/* src/sema.c — реализация семантического анализа. */
#include "sema.h"
#include "diag.h"
#include "format.h"
#include "types.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SymTab *tab;
    Function *fn;         /* разбираемая функция          */
    const Type *ret_type; /* её тип результата            */
    int loop_depth;       /* глубина вложенности циклов   */
    int switch_depth;     /* глубина вложенности switch   */
    int in_scanf_arg;     /* разрешено ли взятие адреса   */
} Sema;

/* stmt_level: выражение стоит на месте инструкции, поэтому
 * инкремент и декремент в нём допустимы. */
static const Type *check_expr(Sema *s, Expr *e, int stmt_level);
static void check_stmt(Sema *s, Stmt *st);

/* --- размеры типов --- */

int sema_type_size(const Type *t)
{
    if (t == NULL) return 0;
    switch (t->kind) {
    case TY_CHAR:   return 1;
    case TY_SHORT:  return 2;
    case TY_INT:    return 4;
    case TY_LONG:   return 8;
    case TY_FLOAT:  return 4;
    case TY_DOUBLE: return 8;
    case TY_ARRAY: {
        int len = type_array_length(t);
        if (len == TYPE_LENGTH_UNKNOWN) return 0;
        return len * sema_type_size(type_element(t));
    }
    default: return 0;
    }
}

/* --- вычисление константных выражений --- */

int sema_const_int(const Expr *e, long long *out)
{
    long long a, b;

    if (e == NULL) return 0;

    switch (e->kind) {
    case EX_INT_LIT:
    case EX_CHAR_LIT:
        *out = e->u.ival;
        return 1;

    case EX_UNARY:
        if (!sema_const_int(e->u.un.operand, &a)) return 0;
        switch (e->u.un.op) {
        case OP_NEG:  *out = -a; return 1;
        case OP_PLUS: *out = a; return 1;
        case OP_BNOT: *out = ~a; return 1;
        case OP_NOT:  *out = !a; return 1;
        default: return 0;
        }

    case EX_CAST:
        if (!type_is_integer(e->u.cast.to)) return 0;
        return sema_const_int(e->u.cast.operand, out);

    case EX_SIZEOF:
        if (e->u.size_of.of_type != NULL)
            *out = sema_type_size(e->u.size_of.of_type);
        else if (e->u.size_of.operand != NULL)
            *out = sema_type_size(e->u.size_of.operand->type);
        else
            return 0;
        return 1;

    case EX_BINARY:
        if (!sema_const_int(e->u.bin.lhs, &a)) return 0;
        if (!sema_const_int(e->u.bin.rhs, &b)) return 0;
        switch (e->u.bin.op) {
        case OP_ADD: *out = a + b; return 1;
        case OP_SUB: *out = a - b; return 1;
        case OP_MUL: *out = a * b; return 1;
        case OP_DIV: if (b == 0) return 0; *out = a / b; return 1;
        case OP_MOD: if (b == 0) return 0; *out = a % b; return 1;
        case OP_SHL: *out = a << b; return 1;
        case OP_SHR: *out = a >> b; return 1;
        case OP_BAND: *out = a & b; return 1;
        case OP_BOR:  *out = a | b; return 1;
        case OP_BXOR: *out = a ^ b; return 1;
        case OP_LT: *out = a < b; return 1;
        case OP_GT: *out = a > b; return 1;
        case OP_LE: *out = a <= b; return 1;
        case OP_GE: *out = a >= b; return 1;
        case OP_EQ: *out = a == b; return 1;
        case OP_NE: *out = a != b; return 1;
        default: return 0;
        }

    default:
        return 0;
    }
}

/* --- встроенные функции --- */

typedef struct {
    const char *name;
    const Type *(*ret)(void);
    int nparams;
    int variadic;
} BuiltinDesc;

static void register_builtins(Sema *s)
{
    static const BuiltinDesc table[] = {
        {"printf",  type_int,    1, 1},
        {"scanf",   type_int,    1, 1},
        {"puts",    type_int,    1, 0},
        {"putchar", type_int,    1, 0},
        {"getchar", type_int,    0, 0},
        {"abs",     type_int,    1, 0},
        {"labs",    type_long,   1, 0},
        {"fabs",    type_double, 1, 0},
        {"sqrt",    type_double, 1, 0},
        {"pow",     type_double, 2, 0},
        {"floor",   type_double, 1, 0},
        {"ceil",    type_double, 1, 0},
        {"exit",    type_void,   1, 0},
        {"rand",    type_int,    0, 0},
        {"srand",   type_void,   1, 0},
        {"strlen",  type_int,    1, 0},
        {NULL, NULL, 0, 0}};

    SrcPos nowhere;
    int i;

    nowhere.line = 0;
    nowhere.col = 0;

    for (i = 0; table[i].name != NULL; i++) {
        Symbol *sym = symtab_declare(s->tab, table[i].name, SYM_FUNC,
                                     table[i].ret(), nowhere);
        if (sym == NULL) continue;
        sym->is_builtin = 1;
        sym->is_variadic = table[i].variadic;
        sym->has_body = 1;
        for (; sym->nparams < table[i].nparams;)
            symtab_add_param_type(sym, type_int());
    }

    /* Имена служебных функций порождаемого кода резервируются,
     * чтобы переменная пользователя их не перекрыла. */
    symtab_reserve(s->tab, "_c_div");
    symtab_reserve(s->tab, "_c_mod");
    symtab_reserve(s->tab, "_c_input");
}

/* --- вспомогательное --- */

static int is_lvalue(const Expr *e)
{
    if (e == NULL) return 0;
    if (e->kind == EX_IDENT) return e->u.ident.sym == NULL ||
                                    e->u.ident.sym->kind != SYM_FUNC;
    return e->kind == EX_INDEX;
}

/* Требует, чтобы выражение можно было использовать как условие. */
static void require_scalar(Sema *s, const Expr *e, const char *what)
{
    if (e == NULL) return;
    if (type_is_error(e->type)) return;
    if (!type_is_arithmetic(e->type))
        diag_error_at(e->pos, "%s must be a number, got '%s'", what,
                      type_name(e->type));
}

/* --- проверка вызовов --- */

static const Type *check_printf(Sema *s, Expr *e)
{
    Expr *fmt_arg;
    FmtString *fmt;
    int expected, given;

    if (e->u.call.nargs < 1) {
        diag_error_at(e->pos, "printf requires a format string");
        return type_int();
    }

    fmt_arg = e->u.call.args[0];
    if (fmt_arg->kind != EX_STR_LIT) {
        diag_error_at(fmt_arg->pos,
                      "the format string must be a literal");
        return type_int();
    }

    fmt = fmt_parse(fmt_arg->u.str.data, fmt_arg->u.str.len);
    if (fmt_has_unsupported(fmt))
        diag_error_at(fmt_arg->pos, "conversion '%s' is not supported",
                      fmt_unsupported_spec(fmt));

    expected = fmt_arg_count(fmt);
    given = e->u.call.nargs - 1;
    if (expected != given)
        diag_error_at(e->pos,
                      "format string needs %d argument(s), but %d given",
                      expected, given);
    fmt_free(fmt);
    return type_int();
}

static const Type *check_scanf(Sema *s, Expr *e)
{
    Expr *fmt_arg;
    FmtString *fmt;
    int expected, given, i;

    if (e->u.call.nargs < 1) {
        diag_error_at(e->pos, "scanf requires a format string");
        return type_int();
    }

    fmt_arg = e->u.call.args[0];
    if (fmt_arg->kind != EX_STR_LIT) {
        diag_error_at(fmt_arg->pos, "the format string must be a literal");
        return type_int();
    }

    fmt = fmt_parse(fmt_arg->u.str.data, fmt_arg->u.str.len);
    if (fmt_has_unsupported(fmt))
        diag_error_at(fmt_arg->pos, "conversion '%s' is not supported",
                      fmt_unsupported_spec(fmt));

    expected = fmt_arg_count(fmt);
    given = e->u.call.nargs - 1;
    if (expected != given)
        diag_error_at(e->pos,
                      "format string needs %d argument(s), but %d given",
                      expected, given);
    fmt_free(fmt);

    /* Каждый аргумент, кроме массивов, должен передаваться по адресу. */
    for (i = 1; i < e->u.call.nargs; i++) {
        Expr *arg = e->u.call.args[i];
        int by_address = (arg->kind == EX_UNARY && arg->u.un.op == OP_ADDR);
        if (!by_address && !type_is_array(arg->type))
            diag_error_at(arg->pos,
                          "scanf argument must be passed by address, "
                          "write '&' before the variable name");
    }
    return type_int();
}

static const Type *check_call(Sema *s, Expr *e, int stmt_level)
{
    Symbol *sym;
    int i;

    /* printf и scanf проверяются по строке формата. */
    if (strcmp(e->u.call.name, "printf") == 0 ||
        strcmp(e->u.call.name, "scanf") == 0) {
        int is_scanf = (e->u.call.name[0] == 's');
        for (i = 0; i < e->u.call.nargs; i++) {
            s->in_scanf_arg = is_scanf;
            check_expr(s, e->u.call.args[i], 0);
            s->in_scanf_arg = 0;
        }
        /* scanf раскрывается в присваивания и потому не может
         * стоять внутри другого выражения. */
        if (is_scanf && !stmt_level)
            diag_error_at(e->pos,
                          "scanf is only supported as a separate "
                          "statement");
        return is_scanf ? check_scanf(s, e) : check_printf(s, e);
    }

    for (i = 0; i < e->u.call.nargs; i++) check_expr(s, e->u.call.args[i], 0);

    sym = symtab_lookup(s->tab, e->u.call.name);
    if (sym == NULL) {
        diag_error_at(e->pos, "call to undeclared function '%s'",
                      e->u.call.name);
        return type_error();
    }
    if (sym->kind != SYM_FUNC) {
        diag_error_at(e->pos, "'%s' is not a function", e->u.call.name);
        return type_error();
    }

    sym->used = 1;
    if (!sym->is_variadic && e->u.call.nargs != sym->nparams)
        diag_error_at(e->pos,
                      "function '%s' expects %d argument(s), but %d given",
                      e->u.call.name, sym->nparams, e->u.call.nargs);

    return sym->type;
}

/* --- проверка выражений --- */

static const Type *check_binary(Sema *s, Expr *e)
{
    const Type *lt = check_expr(s, e->u.bin.lhs, 0);
    const Type *rt = check_expr(s, e->u.bin.rhs, 0);
    Op op = e->u.bin.op;

    if (type_is_error(lt) || type_is_error(rt)) return type_error();

    switch (op) {
    case OP_MOD:
    case OP_BAND:
    case OP_BOR:
    case OP_BXOR:
    case OP_SHL:
    case OP_SHR:
        if (!type_is_integer(lt) || !type_is_integer(rt)) {
            diag_error_at(e->pos,
                          "operator '%s' requires integer operands, "
                          "got '%s' and '%s'",
                          op_spelling(op), type_name(lt), type_name(rt));
            return type_error();
        }
        return type_usual_arith(lt, rt);

    case OP_LAND:
    case OP_LOR:
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            diag_error_at(e->pos, "operator '%s' requires numeric operands",
                          op_spelling(op));
            return type_error();
        }
        return type_int();

    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
    case OP_EQ:
    case OP_NE:
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            diag_error_at(e->pos, "operator '%s' requires numeric operands",
                          op_spelling(op));
            return type_error();
        }
        return type_int(); /* результат сравнения в C — int */

    default:
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            diag_error_at(e->pos,
                          "operator '%s' requires numeric operands, "
                          "got '%s' and '%s'",
                          op_spelling(op), type_name(lt), type_name(rt));
            return type_error();
        }
        return type_usual_arith(lt, rt);
    }
}

static const Type *check_assign(Sema *s, Expr *e, int stmt_level)
{
    const Type *tt = check_expr(s, e->u.assign.target, 0);
    /* Цепочка "a = b = 0" выражается и в Python, поэтому вложенное
     * присваивание в правой части остаётся допустимым. */
    int chained = (e->u.assign.value->kind == EX_ASSIGN);
    const Type *vt = check_expr(s, e->u.assign.value,
                                chained ? stmt_level : 0);

    /* В Python присваивание — инструкция, а не выражение. */
    if (!stmt_level) {
        diag_error_at(e->pos,
                      "assignment is only supported as a separate "
                      "statement, not inside another expression");
        return type_error();
    }

    if (!is_lvalue(e->u.assign.target)) {
        diag_error_at(e->pos, "left side of assignment is not an lvalue");
        return type_error();
    }
    if (type_is_error(tt) || type_is_error(vt)) return type_error();

    if (type_is_array(tt)) {
        diag_error_at(e->pos, "an array cannot be assigned as a whole");
        return type_error();
    }
    if (!type_assignable(tt, vt)) {
        diag_error_at(e->pos, "cannot assign '%s' to '%s'", type_name(vt),
                      type_name(tt));
        return type_error();
    }

    /* Составное присваивание наследует ограничения своей операции. */
    if (e->u.assign.op == OP_MOD || e->u.assign.op == OP_BAND ||
        e->u.assign.op == OP_BOR || e->u.assign.op == OP_BXOR ||
        e->u.assign.op == OP_SHL || e->u.assign.op == OP_SHR) {
        if (!type_is_integer(tt) || !type_is_integer(vt)) {
            diag_error_at(e->pos,
                          "operator '%s=' requires integer operands",
                          op_spelling(e->u.assign.op));
            return type_error();
        }
    }

    if (e->u.assign.target->kind == EX_IDENT &&
        e->u.assign.target->u.ident.sym != NULL)
        e->u.assign.target->u.ident.sym->assigned = 1;

    return tt;
}

static const Type *check_incdec(Sema *s, Expr *e, int stmt_level)
{
    const Type *t = check_expr(s, e->u.incdec.operand, 0);
    const char *spell =
        (e->kind == EX_PREINC || e->kind == EX_POSTINC) ? "++" : "--";

    if (!is_lvalue(e->u.incdec.operand)) {
        diag_error_at(e->pos, "operand of '%s' is not an lvalue", spell);
        return type_error();
    }

    /* В Python нет "++". Перенос его внутрь выражения потребовал бы
     * временных переменных и менял бы порядок вычислений, поэтому
     * поддерживается только позиция отдельной инструкции. */
    if (!stmt_level) {
        diag_error_at(e->pos,
                      "'%s' is only supported as a separate statement or as "
                      "the third clause of 'for'",
                      spell);
        return type_error();
    }

    if (type_is_error(t)) return type_error();
    if (!type_is_arithmetic(t)) {
        diag_error_at(e->pos, "operand of '%s' must be a number", spell);
        return type_error();
    }

    if (e->u.incdec.operand->kind == EX_IDENT &&
        e->u.incdec.operand->u.ident.sym != NULL)
        e->u.incdec.operand->u.ident.sym->assigned = 1;
    return t;
}

static const Type *check_expr(Sema *s, Expr *e, int stmt_level)
{
    const Type *t = type_error();

    if (e == NULL) return type_error();

    switch (e->kind) {
    case EX_INT_LIT:
        t = type_int();
        break;
    case EX_CHAR_LIT:
        /* В C символьный литерал имеет тип int. */
        t = type_int();
        break;
    case EX_FLOAT_LIT:
        t = type_double();
        break;
    case EX_STR_LIT:
        t = type_array(type_char(), e->u.str.len + 1);
        break;

    case EX_IDENT: {
        Symbol *sym = symtab_lookup(s->tab, e->u.ident.name);
        if (sym == NULL) {
            diag_error_at(e->pos, "undeclared identifier '%s'",
                          e->u.ident.name);
            t = type_error();
        } else {
            e->u.ident.sym = sym;
            sym->used = 1;
            t = sym->type;
        }
        break;
    }

    case EX_BINARY:
        t = check_binary(s, e);
        break;

    case EX_UNARY:
        if (e->u.un.op == OP_ADDR) {
            const Type *inner = check_expr(s, e->u.un.operand, 0);
            if (!s->in_scanf_arg) {
                diag_error_at(e->pos,
                              "the '&' operator is only supported for scanf "
                              "arguments");
                t = type_error();
            } else if (!is_lvalue(e->u.un.operand)) {
                diag_error_at(e->pos, "operand of '&' is not an lvalue");
                t = type_error();
            } else {
                t = inner;
            }
        } else {
            const Type *inner = check_expr(s, e->u.un.operand, 0);
            if (type_is_error(inner)) {
                t = type_error();
            } else if (!type_is_arithmetic(inner)) {
                diag_error_at(e->pos, "operator '%s' requires a number",
                              op_spelling(e->u.un.op));
                t = type_error();
            } else if (e->u.un.op == OP_NOT) {
                t = type_int();
            } else if (e->u.un.op == OP_BNOT) {
                if (!type_is_integer(inner)) {
                    diag_error_at(e->pos, "operator '~' requires an integer");
                    t = type_error();
                } else {
                    t = type_promote(inner);
                }
            } else {
                t = type_promote(inner);
            }
        }
        break;

    case EX_ASSIGN:
        t = check_assign(s, e, stmt_level);
        break;

    case EX_COND: {
        const Type *a, *b;
        check_expr(s, e->u.cond.cond, 0);
        require_scalar(s, e->u.cond.cond, "condition of '?:'");
        a = check_expr(s, e->u.cond.then_expr, 0);
        b = check_expr(s, e->u.cond.else_expr, 0);
        t = type_usual_arith(a, b);
        break;
    }

    case EX_CALL:
        t = check_call(s, e, stmt_level);
        break;

    case EX_INDEX: {
        const Type *base = check_expr(s, e->u.index.base, 0);
        const Type *idx = check_expr(s, e->u.index.index, 0);
        if (type_is_error(base) || type_is_error(idx)) {
            t = type_error();
        } else if (!type_is_array(base)) {
            diag_error_at(e->pos, "subscripted value of type '%s' is not "
                                  "an array",
                          type_name(base));
            t = type_error();
        } else if (!type_is_integer(idx)) {
            diag_error_at(e->pos, "array index must be an integer, got '%s'",
                          type_name(idx));
            t = type_error();
        } else {
            t = type_element(base);
        }
        break;
    }

    case EX_PREINC:
    case EX_PREDEC:
    case EX_POSTINC:
    case EX_POSTDEC:
        t = check_incdec(s, e, stmt_level);
        break;

    case EX_CAST: {
        const Type *inner = check_expr(s, e->u.cast.operand, 0);
        if (type_is_error(inner)) {
            t = type_error();
        } else if (!type_is_arithmetic(e->u.cast.to)) {
            diag_error_at(e->pos, "cannot cast to '%s'",
                          type_name(e->u.cast.to));
            t = type_error();
        } else {
            t = e->u.cast.to;
        }
        break;
    }

    case EX_COMMA: {
        int i;
        for (i = 0; i < e->u.comma.count; i++)
            t = check_expr(s, e->u.comma.items[i], stmt_level);
        break;
    }

    case EX_SIZEOF:
        if (e->u.size_of.operand != NULL) check_expr(s, e->u.size_of.operand, 0);
        t = type_int();
        break;
    }

    e->type = t;
    return t;
}

/* --- объявления --- */

static void declare_variables(Sema *s, Stmt *st)
{
    int i;
    for (i = 0; i < st->u.decl.count; i++) {
        Declarator *d = st->u.decl.items[i];
        Symbol *sym;

        if (type_is_void(d->type)) {
            diag_error_at(d->pos, "variable '%s' cannot have type void",
                          d->name);
            continue;
        }

        /* Инициализатор вычисляется до объявления: в C имя становится
         * видимым только после своего описателя. */
        if (d->init != NULL) check_expr(s, d->init, 0);
        if (d->has_init_list) {
            int k;
            for (k = 0; k < d->ninit; k++) check_expr(s, d->init_list[k], 0);
        }

        sym = symtab_declare(s->tab, d->name, SYM_VAR, d->type, d->pos);
        if (sym == NULL) {
            diag_error_at(d->pos, "redeclaration of '%s'", d->name);
            continue;
        }
        d->sym = sym;

        if (d->init != NULL) {
            sym->assigned = 1;
            if (type_is_array(d->type) &&
                type_element(d->type) == type_char() &&
                d->init->kind == EX_STR_LIT) {
                /* char msg[] = "..." — массив символов, заданный строкой.
                 * В порождаемом коде это обычная строка Python. */
                sym->is_py_string = 1;
                if (type_array_length(d->type) == TYPE_LENGTH_UNKNOWN) {
                    d->type = type_array(type_char(), d->init->u.str.len + 1);
                    sym->type = d->type;
                }
            } else if (type_is_array(d->type))
                diag_error_at(d->pos,
                              "an array needs a braced initializer list");
            else if (!type_assignable(d->type, d->init->type))
                diag_error_at(d->pos, "cannot initialize '%s' with '%s'",
                              type_name(d->type), type_name(d->init->type));
        }

        if (d->has_init_list) {
            sym->assigned = 1;
            if (!type_is_array(d->type)) {
                diag_error_at(d->pos,
                              "a braced initializer list needs an array");
            } else {
                int len = type_array_length(d->type);
                if (len != TYPE_LENGTH_UNKNOWN && d->ninit > len)
                    diag_error_at(d->pos,
                                  "too many initializers: array holds %d, "
                                  "but %d given",
                                  len, d->ninit);
            }
        }
    }
}

/* --- switch --- */

/* Ищет break, относящийся к текущему switch. Вложенные циклы и switch
 * перехватывают break на себя, поэтому внутрь них спускаться не нужно. */
static int has_switch_break(Stmt *st)
{
    int i;
    if (st == NULL) return 0;
    switch (st->kind) {
    case ST_BREAK:
        return 1;
    case ST_BLOCK:
        for (i = 0; i < st->u.block.count; i++)
            if (has_switch_break(st->u.block.items[i])) return 1;
        return 0;
    case ST_IF:
        return has_switch_break(st->u.if_s.then_branch) ||
               has_switch_break(st->u.if_s.else_branch);
    default:
        return 0; /* циклы и вложенный switch перехватывают break */
    }
}

/* Ветвь завершена, если управление гарантированно не пойдёт дальше. */
static int branch_terminates(Stmt *st)
{
    if (st == NULL) return 0;
    switch (st->kind) {
    case ST_BREAK:
    case ST_RETURN:
    case ST_CONTINUE:
        return 1;
    case ST_BLOCK:
        if (st->u.block.count == 0) return 0;
        return branch_terminates(st->u.block.items[st->u.block.count - 1]);
    case ST_IF:
        return branch_terminates(st->u.if_s.then_branch) &&
               branch_terminates(st->u.if_s.else_branch);
    default:
        return 0;
    }
}

static void check_switch(Sema *s, Stmt *st)
{
    SwitchCase *sc;
    long long seen[256];
    int nseen = 0;
    int default_count = 0;

    check_expr(s, st->u.switch_s.subject, 0);
    if (!type_is_error(st->u.switch_s.subject->type) &&
        !type_is_integer(st->u.switch_s.subject->type))
        diag_error_at(st->u.switch_s.subject->pos,
                      "switch requires an integer value, got '%s'",
                      type_name(st->u.switch_s.subject->type));

    s->switch_depth++;
    for (sc = st->u.switch_s.cases; sc != NULL; sc = sc->next) {
        int i;

        if (sc->is_default && ++default_count > 1)
            diag_error_at(sc->pos, "duplicate 'default' branch");

        for (i = 0; i < sc->nlabels; i++) {
            long long value;
            check_expr(s, sc->labels[i], 0);
            if (!sema_const_int(sc->labels[i], &value)) {
                diag_error_at(sc->labels[i]->pos,
                              "case label must be an integer constant");
                continue;
            }
            {
                int k, dup = 0;
                for (k = 0; k < nseen; k++)
                    if (seen[k] == value) dup = 1;
                if (dup)
                    diag_error_at(sc->labels[i]->pos,
                                  "duplicate case label %ld", (long)value);
                else if (nseen < 256)
                    seen[nseen++] = value;
            }
        }

        for (i = 0; i < sc->body.count; i++) check_stmt(s, sc->body.items[i]);

        /* break в середине ветви обрывает её досрочно, а цепочка
         * if/elif в Python так не умеет. */
        for (i = 0; i + 1 < sc->body.count; i++)
            if (has_switch_break(sc->body.items[i])) {
                diag_error_at(sc->body.items[i]->pos,
                              "'break' is only supported as the last "
                              "statement of a switch branch");
                break;
            }

        /* Провал в следующую ветвь непереводим в if/elif. */
        if (sc->next != NULL && sc->body.count > 0 &&
            !branch_terminates(sc->body.items[sc->body.count - 1]))
            diag_error_at(sc->pos,
                          "fall-through between switch branches is not "
                          "supported: add 'break'");
    }
    s->switch_depth--;
}

/* --- инструкции --- */

static void check_stmt(Sema *s, Stmt *st)
{
    int i;
    if (st == NULL) return;

    switch (st->kind) {
    case ST_EMPTY:
    case ST_BREAK:
    case ST_CONTINUE:
        if (st->kind == ST_BREAK && s->loop_depth == 0 && s->switch_depth == 0)
            diag_error_at(st->pos, "'break' outside of a loop or switch");
        if (st->kind == ST_CONTINUE && s->loop_depth == 0)
            diag_error_at(st->pos, "'continue' outside of a loop");
        break;

    case ST_EXPR:
        check_expr(s, st->u.expr, 1); /* инкремент здесь допустим */
        break;

    case ST_DECL:
        declare_variables(s, st);
        break;

    case ST_BLOCK:
        symtab_push(s->tab);
        for (i = 0; i < st->u.block.count; i++)
            check_stmt(s, st->u.block.items[i]);
        symtab_pop(s->tab);
        break;

    case ST_IF:
        check_expr(s, st->u.if_s.cond, 0);
        require_scalar(s, st->u.if_s.cond, "condition of 'if'");
        check_stmt(s, st->u.if_s.then_branch);
        check_stmt(s, st->u.if_s.else_branch);
        break;

    case ST_WHILE:
        check_expr(s, st->u.while_s.cond, 0);
        require_scalar(s, st->u.while_s.cond, "condition of 'while'");
        s->loop_depth++;
        check_stmt(s, st->u.while_s.body);
        s->loop_depth--;
        break;

    case ST_DO:
        s->loop_depth++;
        check_stmt(s, st->u.do_s.body);
        s->loop_depth--;
        check_expr(s, st->u.do_s.cond, 0);
        require_scalar(s, st->u.do_s.cond, "condition of 'do-while'");
        break;

    case ST_FOR:
        /* Объявление в заголовке видно только внутри цикла. */
        symtab_push(s->tab);
        check_stmt(s, st->u.for_s.init);
        if (st->u.for_s.cond != NULL) {
            check_expr(s, st->u.for_s.cond, 0);
            require_scalar(s, st->u.for_s.cond, "condition of 'for'");
        }
        /* Третья часть заголовка — позиция инструкции. */
        if (st->u.for_s.step != NULL) check_expr(s, st->u.for_s.step, 1);
        s->loop_depth++;
        check_stmt(s, st->u.for_s.body);
        s->loop_depth--;
        symtab_pop(s->tab);
        break;

    case ST_SWITCH:
        check_switch(s, st);
        break;

    case ST_RETURN:
        if (st->u.expr == NULL) {
            if (!type_is_void(s->ret_type))
                diag_error_at(st->pos,
                              "'return' without a value in a function "
                              "returning '%s'",
                              type_name(s->ret_type));
        } else {
            const Type *vt = check_expr(s, st->u.expr, 0);
            if (type_is_void(s->ret_type))
                diag_error_at(st->pos,
                              "'return' with a value in a function "
                              "returning void");
            else if (!type_is_error(vt) && !type_assignable(s->ret_type, vt))
                diag_error_at(st->pos, "cannot return '%s' from a function "
                                       "returning '%s'",
                              type_name(vt), type_name(s->ret_type));
        }
        break;
    }
}

/* --- функции --- */

/* Первый проход: объявляет все функции, чтобы вызов мог стоять
 * раньше определения. */
static void collect_functions(Sema *s, Program *prog)
{
    int i, k;
    for (i = 0; i < prog->count; i++) {
        Function *fn;
        Symbol *sym;

        if (prog->items[i].kind != TL_FUNC) continue;
        fn = prog->items[i].u.func;

        sym = symtab_lookup(s->tab, fn->name);
        if (sym != NULL) {
            /* Повторное объявление допустимо, если это прототип
             * и следующее за ним определение. */
            if (sym->kind != SYM_FUNC) {
                diag_error_at(fn->pos, "redeclaration of '%s'", fn->name);
                continue;
            }
            if (sym->has_body && fn->body != NULL) {
                diag_error_at(fn->pos, "redefinition of function '%s'",
                              fn->name);
                continue;
            }
            if (sym->nparams != fn->nparams)
                diag_error_at(fn->pos,
                              "declaration of '%s' does not match the "
                              "previous one",
                              fn->name);
            if (fn->body != NULL) sym->has_body = 1;
            continue;
        }

        sym = symtab_declare(s->tab, fn->name, SYM_FUNC, fn->ret_type, fn->pos);
        if (sym == NULL) continue;
        sym->has_body = (fn->body != NULL);
        for (k = 0; k < fn->nparams; k++)
            symtab_add_param_type(sym, fn->params[k].type);
    }
}

static void check_function(Sema *s, Function *fn)
{
    int i;

    if (fn->body == NULL) return; /* прототип проверять нечего */

    s->fn = fn;
    s->ret_type = fn->ret_type;
    s->loop_depth = 0;
    s->switch_depth = 0;

    symtab_begin_function(s->tab);
    symtab_push(s->tab);

    for (i = 0; i < fn->nparams; i++) {
        Symbol *sym;
        if (type_is_void(fn->params[i].type)) {
            diag_error_at(fn->params[i].pos, "parameter cannot have type void");
            continue;
        }
        sym = symtab_declare(s->tab, fn->params[i].name, SYM_PARAM,
                             fn->params[i].type, fn->params[i].pos);
        if (sym == NULL) {
            diag_error_at(fn->params[i].pos, "duplicate parameter '%s'",
                          fn->params[i].name);
            continue;
        }
        sym->assigned = 1;
        fn->params[i].sym = sym;
    }

    /* Тело — это блок, но собственную область он не открывает:
     * параметры и переменные верхнего уровня живут вместе. */
    for (i = 0; i < fn->body->u.block.count; i++)
        check_stmt(s, fn->body->u.block.items[i]);

    symtab_pop(s->tab);
    symtab_end_function(s->tab);
    s->fn = NULL;
}

int sema_check(Program *prog, SymTab *tab)
{
    Sema s;
    Symbol *main_sym;
    int i;

    s.tab = tab;
    s.fn = NULL;
    s.ret_type = type_void();
    s.loop_depth = 0;
    s.switch_depth = 0;
    s.in_scanf_arg = 0;

    register_builtins(&s);
    collect_functions(&s, prog);

    for (i = 0; i < prog->count; i++) {
        if (prog->items[i].kind == TL_VAR)
            declare_variables(&s, prog->items[i].u.var_decl);
        else
            check_function(&s, prog->items[i].u.func);
    }

    main_sym = symtab_lookup(tab, "main");
    if (main_sym == NULL || main_sym->kind != SYM_FUNC || !main_sym->has_body) {
        SrcPos nowhere;
        nowhere.line = 0;
        nowhere.col = 0;
        diag_error_at(nowhere, "the program has no 'main' function");
    }

    return !diag_has_errors();
}
