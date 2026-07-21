/* src/codegen.c — генерация кода на языке Python. */
#include "codegen.h"
#include "comments.h"
#include "diag.h"
#include "escape.h"
#include "format.h"
#include "sema.h"
#include "strbuf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Приоритеты операций Python — они отличаются от C: сравнение там
 * связывает слабее побитовых операций, а "not" слабее сравнения. */
enum {
    PREC_TERNARY = 1,
    PREC_OR = 2,
    PREC_AND = 3,
    PREC_NOT = 4,
    PREC_CMP = 5,
    PREC_BOR = 6,
    PREC_BXOR = 7,
    PREC_BAND = 8,
    PREC_SHIFT = 9,
    PREC_ADD = 10,
    PREC_MUL = 11,
    PREC_UNARY = 12,
    PREC_ATOM = 14
};

typedef struct {
    StrBuf out;
    int indent;
    SymTab *tab;

    /* какие части служебной части понадобились */
    int need_div, need_mod, need_sys, need_math, need_random, need_input;

    /* Шаг цикла for, развёрнутого в while: его нужно повторить
     * перед каждым continue, иначе цикл станет бесконечным. */
    Expr *pending_step;

    int in_switch;     /* break относится к switch, а не к циклу */
    /* Значение логической операции используется как условие,
     * а не как число: тогда обёртка int(bool(...)) не нужна. */
    int cond_depth;
    int temp_counter;  /* номер служебной переменной switch      */
    int comment_index; /* следующий невыведенный комментарий     */
} Gen;

static void gen_expr(Gen *g, const Expr *e, int min_prec);
static void gen_stmt(Gen *g, Stmt *st);
static void gen_condition(Gen *g, const Expr *e, int min_prec);

/* --- вывод с отступами --- */

static void indent_line(Gen *g)
{
    int i;
    for (i = 0; i < g->indent; i++) sb_puts(&g->out, "    ");
}

static void line_start(Gen *g, int src_line);

/* --- комментарии --- */

/* Выводит комментарии, которые в исходном тексте стоят выше
 * очередной инструкции. */
static void flush_comments_before(Gen *g, int src_line)
{
    while (g->comment_index < comments_count()) {
        const Comment *c = comments_get(g->comment_index);
        const char *text;
        const char *nl;

        if (src_line > 0 && c->line >= src_line) break;
        g->comment_index++;

        text = c->text;
        /* Блочный комментарий может занимать несколько строк. */
        for (;;) {
            nl = strchr(text, '\n');
            indent_line(g);
            sb_puts(&g->out, "#");
            if (nl != NULL) {
                sb_write(&g->out, text, (size_t)(nl - text));
                sb_putc(&g->out, '\n');
                text = nl + 1;
            } else {
                sb_puts(&g->out, text);
                sb_putc(&g->out, '\n');
                break;
            }
        }
    }
}

static void line_start(Gen *g, int src_line)
{
    flush_comments_before(g, src_line);
    indent_line(g);
}

/* Дописывает комментарий, стоявший в конце строки исходного текста,
 * в конец только что выведенной строки. Для этого снимается уже
 * выведенный перевод строки. */
static void attach_trailing_comment(Gen *g, int src_line)
{
    while (g->comment_index < comments_count()) {
        const Comment *c = comments_get(g->comment_index);

        if (c->own_line || c->line != src_line) break;
        /* Многострочный комментарий в конец строки не поместится. */
        if (strchr(c->text, '\n') != NULL) break;
        /* Приписывать можно только к непустой выведенной строке. */
        if (sb_len(&g->out) == 0 ||
            sb_cstr(&g->out)[sb_len(&g->out) - 1] != '\n')
            break;

        g->comment_index++;
        sb_chop(&g->out, 1);
        sb_puts(&g->out, "  #");
        sb_puts(&g->out, c->text);
        sb_putc(&g->out, '\n');
    }
}

/* --- имена --- */

static const char *py_name_of(const Symbol *sym, const char *fallback)
{
    return (sym != NULL && sym->py_name != NULL) ? sym->py_name : fallback;
}

/* --- значения по умолчанию ---
 * В C локальная переменная без инициализатора не определена.
 * Транслятор выбирает предсказуемый нуль соответствующего типа —
 * в отличие от исходного варианта, где значение угадывалось по имени. */
static void gen_default_value(Gen *g, const Type *t)
{
    if (type_is_array(t)) {
        int len = type_array_length(t);
        sb_puts(&g->out, "[");
        gen_default_value(g, type_element(t));
        sb_printf(&g->out, "] * %d", len == TYPE_LENGTH_UNKNOWN ? 0 : len);
        return;
    }
    if (type_is_floating(t))
        sb_puts(&g->out, "0.0");
    else
        sb_puts(&g->out, "0");
}

/* --- приоритеты --- */

static int prec_of_binop(Op op)
{
    switch (op) {
    case OP_LOR:  return PREC_OR;
    case OP_LAND: return PREC_AND;
    case OP_LT: case OP_GT: case OP_LE:
    case OP_GE: case OP_EQ: case OP_NE: return PREC_CMP;
    case OP_BOR:  return PREC_BOR;
    case OP_BXOR: return PREC_BXOR;
    case OP_BAND: return PREC_BAND;
    case OP_SHL: case OP_SHR: return PREC_SHIFT;
    case OP_ADD: case OP_SUB: return PREC_ADD;
    case OP_MUL: case OP_DIV: case OP_MOD: return PREC_MUL;
    default: return PREC_ATOM;
    }
}

/* Целочисленное деление превращается в вызов функции — а у вызова
 * приоритет атома. */
static int int_division_needs_helper(const Expr *e);

static int prec_of(Gen *g, const Expr *e)
{
    switch (e->kind) {
    case EX_BINARY:
        if (int_division_needs_helper(e)) return PREC_ATOM;
        /* В значении логическая операция оборачивается в int(bool(...)),
         * а вызов функции — это атом, скобки вокруг него не нужны. */
        if ((e->u.bin.op == OP_LAND || e->u.bin.op == OP_LOR) &&
            g->cond_depth == 0)
            return PREC_ATOM;
        return prec_of_binop(e->u.bin.op);
    case EX_UNARY:
        return e->u.un.op == OP_NOT ? PREC_NOT : PREC_UNARY;
    case EX_COND:
        return PREC_TERNARY;
    default:
        return PREC_ATOM;
    }
}

/* --- деление --- */

static int is_nonneg_const(const Expr *e)
{
    long long v;
    return sema_const_int(e, &v) && v >= 0;
}

/* Деление и остаток в C усекают к нулю, а "//" и "%" в Python
 * округляют вниз: -7 / 2 равно -3 в C и -4 в Python. Разница видна
 * только при отрицательных операндах, поэтому для заведомо
 * неотрицательных значений используется обычная операция. */
static int int_division_needs_helper(const Expr *e)
{
    if (e->kind != EX_BINARY) return 0;
    if (e->u.bin.op != OP_DIV && e->u.bin.op != OP_MOD) return 0;
    if (!type_is_integer(e->type)) return 0;
    return !(is_nonneg_const(e->u.bin.lhs) && is_nonneg_const(e->u.bin.rhs));
}

/* --- выражения --- */

static void gen_binary(Gen *g, const Expr *e)
{
    Op op = e->u.bin.op;
    int prec;
    const char *text;
    /* Буфер обязан быть локальным: генерация левого операнда
     * рекурсивно заходит сюда же и затёрла бы общий буфер. */
    char opbuf[8];

    if (int_division_needs_helper(e)) {
        if (op == OP_DIV) {
            g->need_div = 1;
            sb_puts(&g->out, "_c_div(");
        } else {
            g->need_div = 1;
            g->need_mod = 1;
            sb_puts(&g->out, "_c_mod(");
        }
        gen_expr(g, e->u.bin.lhs, PREC_TERNARY);
        sb_puts(&g->out, ", ");
        gen_expr(g, e->u.bin.rhs, PREC_TERNARY);
        sb_puts(&g->out, ")");
        return;
    }

    /* В C операции && и || дают 0 или 1, а в Python and и or
     * возвращают сам операнд: "7 and 2" равно 2, а не 1. Пока значение
     * используется только как условие, разница незаметна; в остальных
     * случаях результат приводится к числу. */
    if (op == OP_LAND || op == OP_LOR) {
        int as_condition = (g->cond_depth > 0);
        if (!as_condition) sb_puts(&g->out, "int(bool(");
        g->cond_depth++;
        prec = prec_of_binop(op);
        gen_expr(g, e->u.bin.lhs, prec);
        sb_puts(&g->out, op == OP_LAND ? " and " : " or ");
        gen_expr(g, e->u.bin.rhs, prec + 1);
        g->cond_depth--;
        if (!as_condition) sb_puts(&g->out, "))");
        return;
    }

    switch (op) {
    case OP_DIV:
        /* Целое деление неотрицательных значений — это "//". */
        text = type_is_integer(e->type) ? " // " : " / ";
        break;
    default:
        snprintf(opbuf, sizeof(opbuf), " %s ", op_spelling(op));
        text = opbuf;
        break;
    }

    prec = prec_of_binop(op);
    gen_expr(g, e->u.bin.lhs, prec);
    sb_puts(&g->out, text);
    gen_expr(g, e->u.bin.rhs, prec + 1);
}

/* Печатает число с плавающей точкой так, чтобы Python прочитал
 * его как то же самое значение. */
static void gen_double(Gen *g, double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", v);
    sb_puts(&g->out, buf);
    /* Значение без точки и экспоненты стало бы целым. */
    if (strpbrk(buf, ".eEn") == NULL) sb_puts(&g->out, ".0");
}

static void gen_expr_inner(Gen *g, const Expr *e)
{
    int i;

    switch (e->kind) {
    case EX_INT_LIT:
    case EX_CHAR_LIT:
        sb_printf(&g->out, "%ld", (long)e->u.ival);
        break;

    case EX_FLOAT_LIT:
        gen_double(g, e->u.fval);
        break;

    case EX_STR_LIT: {
        char *lit = escape_to_python_string(e->u.str.data, e->u.str.len);
        sb_puts(&g->out, lit);
        free(lit);
        break;
    }

    case EX_IDENT:
        sb_puts(&g->out, py_name_of(e->u.ident.sym, e->u.ident.name));
        break;

    case EX_BINARY:
        gen_binary(g, e);
        break;

    case EX_UNARY:
        switch (e->u.un.op) {
        case OP_NOT:
            /* "not" даёт True/False, а они в арифметике равны 1 и 0,
             * поэтому дополнительного приведения не требуется. */
            sb_puts(&g->out, "not ");
            g->cond_depth++;
            gen_expr(g, e->u.un.operand, PREC_NOT);
            g->cond_depth--;
            break;
        case OP_PLUS:
            /* Унарный плюс на значение не влияет. */
            gen_expr(g, e->u.un.operand, PREC_UNARY);
            break;
        case OP_ADDR:
            /* Встречается только в аргументах scanf,
             * которые обрабатываются отдельно. */
            gen_expr(g, e->u.un.operand, PREC_UNARY);
            break;
        default:
            sb_puts(&g->out, op_spelling(e->u.un.op));
            gen_expr(g, e->u.un.operand, PREC_UNARY);
            break;
        }
        break;

    case EX_COND:
        gen_expr(g, e->u.cond.then_expr, PREC_OR);
        sb_puts(&g->out, " if ");
        gen_condition(g, e->u.cond.cond, PREC_OR);
        sb_puts(&g->out, " else ");
        gen_expr(g, e->u.cond.else_expr, PREC_TERNARY);
        break;

    case EX_INDEX:
        gen_expr(g, e->u.index.base, PREC_ATOM);
        sb_puts(&g->out, "[");
        gen_expr(g, e->u.index.index, PREC_TERNARY);
        sb_puts(&g->out, "]");
        break;

    case EX_CAST:
        /* Приведение к целому усекает к нулю — как int() в Python. */
        if (type_is_integer(e->u.cast.to) &&
            type_is_floating(e->u.cast.operand->type)) {
            sb_puts(&g->out, "int(");
            gen_expr(g, e->u.cast.operand, PREC_TERNARY);
            sb_puts(&g->out, ")");
        } else if (type_is_floating(e->u.cast.to) &&
                   type_is_integer(e->u.cast.operand->type)) {
            sb_puts(&g->out, "float(");
            gen_expr(g, e->u.cast.operand, PREC_TERNARY);
            sb_puts(&g->out, ")");
        } else {
            gen_expr(g, e->u.cast.operand, PREC_ATOM);
        }
        break;

    case EX_SIZEOF: {
        long long size = 0;
        sema_const_int(e, &size);
        sb_printf(&g->out, "%ld", (long)size);
        break;
    }

    case EX_CALL: {
        const char *name = e->u.call.name;
        const char *py = name;
        Symbol *sym = symtab_lookup(g->tab, name);

        /* Отображение стандартных функций C на средства Python. */
        if (strcmp(name, "abs") == 0 || strcmp(name, "labs") == 0 ||
            strcmp(name, "fabs") == 0) {
            py = "abs";
        } else if (strcmp(name, "sqrt") == 0) {
            g->need_math = 1;
            py = "math.sqrt";
        } else if (strcmp(name, "pow") == 0) {
            g->need_math = 1;
            py = "math.pow";
        } else if (strcmp(name, "floor") == 0) {
            g->need_math = 1;
            py = "math.floor";
        } else if (strcmp(name, "ceil") == 0) {
            g->need_math = 1;
            py = "math.ceil";
        } else if (strcmp(name, "exit") == 0) {
            g->need_sys = 1;
            py = "sys.exit";
        } else if (strcmp(name, "strlen") == 0) {
            py = "len";
        } else if (strcmp(name, "rand") == 0) {
            g->need_random = 1;
            sb_puts(&g->out, "random.randint(0, 32767)");
            return;
        } else if (strcmp(name, "srand") == 0) {
            g->need_random = 1;
            py = "random.seed";
        } else if (sym != NULL) {
            py = sym->py_name;
        }

        sb_puts(&g->out, py);
        sb_puts(&g->out, "(");
        for (i = 0; i < e->u.call.nargs; i++) {
            if (i > 0) sb_puts(&g->out, ", ");
            gen_expr(g, e->u.call.args[i], PREC_TERNARY);
        }
        sb_puts(&g->out, ")");
        break;
    }

    case EX_COMMA:
        /* В значении оператор "запятая" не используется:
         * на месте инструкции он разворачивается в отдельные строки. */
        for (i = 0; i < e->u.comma.count; i++) {
            if (i > 0) sb_puts(&g->out, ", ");
            gen_expr(g, e->u.comma.items[i], PREC_TERNARY);
        }
        break;

    case EX_ASSIGN:
    case EX_PREINC:
    case EX_PREDEC:
    case EX_POSTINC:
    case EX_POSTDEC:
        /* Проверено семантическим анализом: эти узлы встречаются
         * только на месте инструкции. */
        sb_puts(&g->out, "<unsupported>");
        break;
    }
}

static void gen_expr(Gen *g, const Expr *e, int min_prec)
{
    int need_parens;
    if (e == NULL) return;
    need_parens = prec_of(g, e) < min_prec;
    if (need_parens) sb_puts(&g->out, "(");
    gen_expr_inner(g, e);
    if (need_parens) sb_puts(&g->out, ")");
}

/* Выражение в позиции условия: его значение нужно лишь для проверки
 * истинности, поэтому логические операции не требуют приведения
 * к числу и выводятся как обычные and/or. */
static void gen_condition(Gen *g, const Expr *e, int min_prec)
{
    g->cond_depth++;
    gen_expr(g, e, min_prec);
    g->cond_depth--;
}

/* --- printf и scanf --- */

static void gen_printf(Gen *g, const Expr *call)
{
    const Expr *fmt_arg = call->u.call.args[0];
    FmtString *fmt = fmt_parse(fmt_arg->u.str.data, fmt_arg->u.str.len);
    int nconv = fmt_conversion_count(fmt);
    int len = 0;
    const char *text = nconv > 0 ? fmt_python(fmt, &len) : fmt_plain(fmt, &len);
    int trailing_newline = (len > 0 && text[len - 1] == '\n');
    char *lit;
    int i;

    /* Перевод строки в конце — это поведение print по умолчанию. */
    if (trailing_newline) len--;

    lit = escape_to_python_string(text, len);
    sb_puts(&g->out, "print(");
    sb_puts(&g->out, lit);
    free(lit);

    if (nconv > 0) {
        sb_puts(&g->out, " % (");
        for (i = 1; i < call->u.call.nargs; i++) {
            if (i > 1) sb_puts(&g->out, ", ");
            gen_expr(g, call->u.call.args[i], PREC_TERNARY);
        }
        /* Кортеж из одного элемента требует запятой. */
        if (call->u.call.nargs == 2) sb_puts(&g->out, ",");
        sb_puts(&g->out, ")");
    }

    if (!trailing_newline) sb_puts(&g->out, ", end=\"\"");
    sb_puts(&g->out, ")\n");
    fmt_free(fmt);
}

/* scanf разворачивается в отдельные присваивания: каждое читает
 * очередную лексему из стандартного ввода. */
static void gen_scanf(Gen *g, const Expr *call, int src_line)
{
    const Expr *fmt_arg = call->u.call.args[0];
    FmtString *fmt = fmt_parse(fmt_arg->u.str.data, fmt_arg->u.str.len);
    int nconv = fmt_conversion_count(fmt);
    int i;

    g->need_input = 1;

    for (i = 0; i < nconv && i + 1 < call->u.call.nargs; i++) {
        const Expr *arg = call->u.call.args[i + 1];
        const Expr *target =
            (arg->kind == EX_UNARY && arg->u.un.op == OP_ADDR)
                ? arg->u.un.operand
                : arg;

        if (i > 0) line_start(g, src_line);
        gen_expr(g, target, PREC_ATOM);
        switch (fmt_conversion_kind(fmt, i)) {
        case CONV_FLOAT:
        case CONV_EXP:
        case CONV_GENERAL:
            sb_puts(&g->out, " = float(_c_input())\n");
            break;
        case CONV_STRING:
            sb_puts(&g->out, " = _c_input()\n");
            break;
        case CONV_CHAR:
            sb_puts(&g->out, " = ord(_c_input()[0])\n");
            break;
        default:
            sb_puts(&g->out, " = int(_c_input())\n");
            break;
        }
    }
    if (nconv == 0) sb_puts(&g->out, "pass\n");
    fmt_free(fmt);
}

/* --- присваивания и инкремент --- */

static void gen_assign(Gen *g, const Expr *e)
{
    Op op = e->u.assign.op;
    const Expr *target = e->u.assign.target;
    const Expr *value = e->u.assign.value;

    if (op == OP_NONE) {
        gen_expr(g, target, PREC_ATOM);
        sb_puts(&g->out, " = ");
        gen_expr(g, value, PREC_TERNARY);
        return;
    }

    /* Составное деление целых нельзя записать как "//=", потому что
     * усечение должно идти к нулю: раскрываем в полную форму. */
    if ((op == OP_DIV || op == OP_MOD) && type_is_integer(e->type) &&
        !(is_nonneg_const(target) && is_nonneg_const(value))) {
        g->need_div = 1;
        if (op == OP_MOD) g->need_mod = 1;
        gen_expr(g, target, PREC_ATOM);
        sb_puts(&g->out, op == OP_DIV ? " = _c_div(" : " = _c_mod(");
        gen_expr(g, target, PREC_TERNARY);
        sb_puts(&g->out, ", ");
        gen_expr(g, value, PREC_TERNARY);
        sb_puts(&g->out, ")");
        return;
    }

    gen_expr(g, target, PREC_ATOM);
    if (op == OP_DIV && type_is_integer(e->type))
        sb_puts(&g->out, " //= ");
    else if (op == OP_LAND || op == OP_LOR)
        sb_puts(&g->out, " = ");
    else
        sb_printf(&g->out, " %s= ", op_spelling(op));
    gen_expr(g, value, PREC_TERNARY);
}

static void gen_incdec(Gen *g, const Expr *e)
{
    int up = (e->kind == EX_PREINC || e->kind == EX_POSTINC);
    gen_expr(g, e->u.incdec.operand, PREC_ATOM);
    sb_puts(&g->out, up ? " += 1" : " -= 1");
}

/* Выражение на месте инструкции: присваивание, инкремент, вызов
 * либо запятая, разворачиваемая в несколько строк. */
static void gen_expr_stmt(Gen *g, const Expr *e, int src_line)
{
    int i;

    if (e == NULL) return;

    switch (e->kind) {
    case EX_COMMA:
        for (i = 0; i < e->u.comma.count; i++) {
            if (i > 0) line_start(g, src_line);
            gen_expr_stmt(g, e->u.comma.items[i], src_line);
        }
        return;

    case EX_ASSIGN:
        gen_assign(g, e);
        sb_putc(&g->out, '\n');
        return;

    case EX_PREINC:
    case EX_PREDEC:
    case EX_POSTINC:
    case EX_POSTDEC:
        gen_incdec(g, e);
        sb_putc(&g->out, '\n');
        return;

    case EX_CALL:
        if (strcmp(e->u.call.name, "printf") == 0 && e->u.call.nargs >= 1) {
            gen_printf(g, e);
            return;
        }
        if (strcmp(e->u.call.name, "scanf") == 0 && e->u.call.nargs >= 1) {
            gen_scanf(g, e, src_line);
            return;
        }
        if (strcmp(e->u.call.name, "puts") == 0 && e->u.call.nargs == 1) {
            sb_puts(&g->out, "print(");
            gen_expr(g, e->u.call.args[0], PREC_TERNARY);
            sb_puts(&g->out, ")\n");
            return;
        }
        if (strcmp(e->u.call.name, "putchar") == 0 && e->u.call.nargs == 1) {
            sb_puts(&g->out, "print(chr(");
            gen_expr(g, e->u.call.args[0], PREC_TERNARY);
            sb_puts(&g->out, "), end=\"\")\n");
            return;
        }
        gen_expr(g, e, PREC_TERNARY);
        sb_putc(&g->out, '\n');
        return;

    default:
        gen_expr(g, e, PREC_TERNARY);
        sb_putc(&g->out, '\n');
        return;
    }
}

/* --- поиск изменяемых переменных --- */

static int expr_assigns_symbol(const Expr *e, const Symbol *sym);

static int stmt_assigns_symbol(const Stmt *st, const Symbol *sym)
{
    int i;
    if (st == NULL) return 0;
    switch (st->kind) {
    case ST_EXPR:
    case ST_RETURN:
        return expr_assigns_symbol(st->u.expr, sym);
    case ST_DECL:
        for (i = 0; i < st->u.decl.count; i++)
            if (expr_assigns_symbol(st->u.decl.items[i]->init, sym)) return 1;
        return 0;
    case ST_BLOCK:
        for (i = 0; i < st->u.block.count; i++)
            if (stmt_assigns_symbol(st->u.block.items[i], sym)) return 1;
        return 0;
    case ST_IF:
        return expr_assigns_symbol(st->u.if_s.cond, sym) ||
               stmt_assigns_symbol(st->u.if_s.then_branch, sym) ||
               stmt_assigns_symbol(st->u.if_s.else_branch, sym);
    case ST_WHILE:
        return expr_assigns_symbol(st->u.while_s.cond, sym) ||
               stmt_assigns_symbol(st->u.while_s.body, sym);
    case ST_DO:
        return expr_assigns_symbol(st->u.do_s.cond, sym) ||
               stmt_assigns_symbol(st->u.do_s.body, sym);
    case ST_FOR:
        return stmt_assigns_symbol(st->u.for_s.init, sym) ||
               expr_assigns_symbol(st->u.for_s.cond, sym) ||
               expr_assigns_symbol(st->u.for_s.step, sym) ||
               stmt_assigns_symbol(st->u.for_s.body, sym);
    case ST_SWITCH: {
        const SwitchCase *sc;
        if (expr_assigns_symbol(st->u.switch_s.subject, sym)) return 1;
        for (sc = st->u.switch_s.cases; sc != NULL; sc = sc->next)
            for (i = 0; i < sc->body.count; i++)
                if (stmt_assigns_symbol(sc->body.items[i], sym)) return 1;
        return 0;
    }
    default:
        return 0;
    }
}

static int target_is_symbol(const Expr *target, const Symbol *sym)
{
    if (target == NULL) return 0;
    if (target->kind == EX_IDENT) return target->u.ident.sym == sym;
    if (target->kind == EX_INDEX) return target_is_symbol(target->u.index.base, sym);
    return 0;
}

static int expr_assigns_symbol(const Expr *e, const Symbol *sym)
{
    int i;
    if (e == NULL) return 0;
    switch (e->kind) {
    case EX_ASSIGN:
        if (target_is_symbol(e->u.assign.target, sym)) return 1;
        return expr_assigns_symbol(e->u.assign.value, sym);
    case EX_PREINC:
    case EX_PREDEC:
    case EX_POSTINC:
    case EX_POSTDEC:
        return target_is_symbol(e->u.incdec.operand, sym);
    case EX_BINARY:
        return expr_assigns_symbol(e->u.bin.lhs, sym) ||
               expr_assigns_symbol(e->u.bin.rhs, sym);
    case EX_UNARY:
        return expr_assigns_symbol(e->u.un.operand, sym);
    case EX_COND:
        return expr_assigns_symbol(e->u.cond.cond, sym) ||
               expr_assigns_symbol(e->u.cond.then_expr, sym) ||
               expr_assigns_symbol(e->u.cond.else_expr, sym);
    case EX_CALL:
        for (i = 0; i < e->u.call.nargs; i++)
            if (expr_assigns_symbol(e->u.call.args[i], sym)) return 1;
        return 0;
    case EX_INDEX:
        return expr_assigns_symbol(e->u.index.base, sym) ||
               expr_assigns_symbol(e->u.index.index, sym);
    case EX_COMMA:
        for (i = 0; i < e->u.comma.count; i++)
            if (expr_assigns_symbol(e->u.comma.items[i], sym)) return 1;
        return 0;
    case EX_CAST:
        return expr_assigns_symbol(e->u.cast.operand, sym);
    default:
        return 0;
    }
}

/* Меняется ли в теле цикла хоть одна переменная, входящая в выражение. */
static int body_touches_expr(const Stmt *body, const Expr *e)
{
    if (e == NULL) return 0;
    switch (e->kind) {
    case EX_IDENT:
        return stmt_assigns_symbol(body, e->u.ident.sym);
    case EX_CALL:
        return 1; /* вызов может иметь побочный эффект */
    case EX_BINARY:
        return body_touches_expr(body, e->u.bin.lhs) ||
               body_touches_expr(body, e->u.bin.rhs);
    case EX_UNARY:
        return body_touches_expr(body, e->u.un.operand);
    case EX_CAST:
        return body_touches_expr(body, e->u.cast.operand);
    case EX_INDEX:
        return 1;
    case EX_INT_LIT:
    case EX_CHAR_LIT:
    case EX_FLOAT_LIT:
    case EX_SIZEOF:
        return 0;
    default:
        return 1;
    }
}

/* --- цикл for --- */

/* Описание цикла, сводимого к range(). */
typedef struct {
    Symbol *var;
    Expr *start;
    Expr *limit;
    int inclusive; /*条件 использует <= или >= */
    long long step;
} RangeLoop;

/* Проверяет, можно ли записать цикл через range().
 *
 * Требования:
 *   - переменная объявлена в самом заголовке (иначе её значение после
 *     цикла в C и в Python различалось бы: C оставляет значение после
 *     последнего приращения, а Python — последнее значение диапазона);
 *   - условие сравнивает эту переменную с границей;
 *   - шаг — приращение на целочисленную константу;
 *   - ни переменная цикла, ни граница не меняются в теле
 *     (в C граница вычисляется заново на каждой итерации,
 *     а range() вычисляет её один раз).
 */
static int analyze_range_loop(Stmt *st, RangeLoop *out)
{
    Stmt *init = st->u.for_s.init;
    Expr *cond = st->u.for_s.cond;
    Expr *step = st->u.for_s.step;
    Declarator *decl;
    Op cmp;

    if (init == NULL || cond == NULL || step == NULL) return 0;
    if (init->kind != ST_DECL || init->u.decl.count != 1) return 0;

    decl = init->u.decl.items[0];
    if (decl->sym == NULL || decl->init == NULL) return 0;
    if (!type_is_integer(decl->type)) return 0;

    if (cond->kind != EX_BINARY) return 0;
    cmp = cond->u.bin.op;
    if (cmp != OP_LT && cmp != OP_LE && cmp != OP_GT && cmp != OP_GE) return 0;
    if (cond->u.bin.lhs->kind != EX_IDENT ||
        cond->u.bin.lhs->u.ident.sym != decl->sym)
        return 0;

    /* Шаг: i++, i--, i += k, i -= k. */
    out->step = 0;
    if (step->kind == EX_POSTINC || step->kind == EX_PREINC) {
        if (!target_is_symbol(step->u.incdec.operand, decl->sym)) return 0;
        out->step = 1;
    } else if (step->kind == EX_POSTDEC || step->kind == EX_PREDEC) {
        if (!target_is_symbol(step->u.incdec.operand, decl->sym)) return 0;
        out->step = -1;
    } else if (step->kind == EX_ASSIGN &&
               (step->u.assign.op == OP_ADD || step->u.assign.op == OP_SUB)) {
        long long k;
        if (!target_is_symbol(step->u.assign.target, decl->sym)) return 0;
        if (!sema_const_int(step->u.assign.value, &k) || k <= 0) return 0;
        out->step = (step->u.assign.op == OP_ADD) ? k : -k;
    } else {
        return 0;
    }

    /* Направление шага должно согласовываться со сравнением. */
    if (out->step > 0 && cmp != OP_LT && cmp != OP_LE) return 0;
    if (out->step < 0 && cmp != OP_GT && cmp != OP_GE) return 0;

    /* Тело не должно менять ни счётчик, ни границу. */
    if (stmt_assigns_symbol(st->u.for_s.body, decl->sym)) return 0;
    if (body_touches_expr(st->u.for_s.body, cond->u.bin.rhs)) return 0;

    out->var = decl->sym;
    out->start = decl->init;
    out->limit = cond->u.bin.rhs;
    out->inclusive = (cmp == OP_LE || cmp == OP_GE);
    return 1;
}

/* --- инструкции --- */

/* Тело управляющей конструкции с отступом; пустое тело
 * заменяется на pass, которого требует синтаксис Python. */
static void gen_suite(Gen *g, Stmt *body)
{
    size_t before;
    g->indent++;
    before = sb_len(&g->out);
    if (body != NULL) gen_stmt(g, body);
    if (sb_len(&g->out) == before) {
        indent_line(g);
        sb_puts(&g->out, "pass\n");
    }
    g->indent--;
}

static void gen_declaration(Gen *g, Stmt *st)
{
    int i, k;
    for (i = 0; i < st->u.decl.count; i++) {
        Declarator *d = st->u.decl.items[i];
        if (i > 0) line_start(g, st->pos.line);
        sb_puts(&g->out, py_name_of(d->sym, d->name));
        sb_puts(&g->out, " = ");

        if (d->has_init_list) {
            int len = type_array_length(d->type);
            sb_puts(&g->out, "[");
            for (k = 0; k < d->ninit; k++) {
                if (k > 0) sb_puts(&g->out, ", ");
                gen_expr(g, d->init_list[k], PREC_TERNARY);
            }
            /* Остаток массива в C обнуляется. */
            for (k = d->ninit; k < len; k++) {
                if (k > 0) sb_puts(&g->out, ", ");
                gen_default_value(g, type_element(d->type));
            }
            sb_puts(&g->out, "]");
        } else if (d->init != NULL) {
            gen_expr(g, d->init, PREC_TERNARY);
        } else {
            gen_default_value(g, d->type);
        }
        sb_putc(&g->out, '\n');
    }
}

static void gen_switch(Gen *g, Stmt *st)
{
    SwitchCase *sc;
    char temp[32];
    int first = 1;
    int saved_switch;

    snprintf(temp, sizeof(temp), "_sw%d", g->temp_counter++);

    /* Значение вычисляется один раз: в C оно тоже вычисляется однажды. */
    sb_puts(&g->out, temp);
    sb_puts(&g->out, " = ");
    gen_expr(g, st->u.switch_s.subject, PREC_TERNARY);
    sb_putc(&g->out, '\n');

    saved_switch = g->in_switch;
    g->in_switch = 1;

    for (sc = st->u.switch_s.cases; sc != NULL; sc = sc->next) {
        int i;
        size_t before;

        line_start(g, sc->pos.line);
        if (sc->is_default && sc->nlabels == 0) {
            sb_puts(&g->out, first ? "if True:\n" : "else:\n");
        } else {
            sb_puts(&g->out, first ? "if " : "elif ");
            for (i = 0; i < sc->nlabels; i++) {
                if (i > 0) sb_puts(&g->out, " or ");
                sb_printf(&g->out, "%s == ", temp);
                gen_expr(g, sc->labels[i], PREC_CMP + 1);
            }
            sb_puts(&g->out, ":\n");
        }
        first = 0;

        g->indent++;
        before = sb_len(&g->out);
        for (i = 0; i < sc->body.count; i++) gen_stmt(g, sc->body.items[i]);
        if (sb_len(&g->out) == before) {
            indent_line(g);
            sb_puts(&g->out, "pass\n");
        }
        g->indent--;
    }

    g->in_switch = saved_switch;
}

static void gen_for(Gen *g, Stmt *st)
{
    RangeLoop range;
    Expr *saved_step;
    int saved_switch = g->in_switch;

    g->in_switch = 0;

    if (analyze_range_loop(st, &range)) {
        indent_line(g);
        sb_printf(&g->out, "for %s in range(", range.var->py_name);
        gen_expr(g, range.start, PREC_TERNARY);
        sb_puts(&g->out, ", ");
        gen_expr(g, range.limit, PREC_ADD);
        /* range не включает верхнюю границу, а "<=" включает. */
        if (range.inclusive) sb_puts(&g->out, range.step > 0 ? " + 1" : " - 1");
        if (range.step != 1) sb_printf(&g->out, ", %ld", (long)range.step);
        sb_puts(&g->out, "):\n");

        saved_step = g->pending_step;
        g->pending_step = NULL;
        gen_suite(g, st->u.for_s.body);
        g->pending_step = saved_step;
        g->in_switch = saved_switch;
        return;
    }

    /* Общий случай: инициализация, while с условием и шаг в конце тела.
     * Инициализация выводится как обычная инструкция и сама расставляет
     * отступ, поэтому здесь он не добавляется. */
    if (st->u.for_s.init != NULL) gen_stmt(g, st->u.for_s.init);

    indent_line(g);
    sb_puts(&g->out, "while ");
    if (st->u.for_s.cond != NULL)
        gen_condition(g, st->u.for_s.cond, PREC_TERNARY);
    else
        sb_puts(&g->out, "True");
    sb_puts(&g->out, ":\n");

    saved_step = g->pending_step;
    g->pending_step = st->u.for_s.step;

    g->indent++;
    {
        size_t before = sb_len(&g->out);
        if (st->u.for_s.body != NULL) gen_stmt(g, st->u.for_s.body);
        if (st->u.for_s.step != NULL) {
            indent_line(g);
            gen_expr_stmt(g, st->u.for_s.step, st->pos.line);
        } else if (sb_len(&g->out) == before) {
            indent_line(g);
            sb_puts(&g->out, "pass\n");
        }
    }
    g->indent--;

    g->pending_step = saved_step;
    g->in_switch = saved_switch;
}

static void gen_stmt(Gen *g, Stmt *st)
{
    int i;
    Expr *saved_step;
    int saved_switch;

    if (st == NULL) return;

    switch (st->kind) {
    case ST_EMPTY:
        break;

    case ST_BLOCK:
        /* В Python нет блочной области видимости, поэтому вложенный
         * блок просто раскрывается на месте. */
        for (i = 0; i < st->u.block.count; i++) gen_stmt(g, st->u.block.items[i]);
        break;

    case ST_DECL:
        line_start(g, st->pos.line);
        gen_declaration(g, st);
        attach_trailing_comment(g, st->pos.line);
        break;

    case ST_EXPR:
        line_start(g, st->pos.line);
        gen_expr_stmt(g, st->u.expr, st->pos.line);
        attach_trailing_comment(g, st->pos.line);
        break;

    case ST_IF:
        line_start(g, st->pos.line);
        sb_puts(&g->out, "if ");
        gen_condition(g, st->u.if_s.cond, PREC_TERNARY);
        sb_puts(&g->out, ":\n");
        gen_suite(g, st->u.if_s.then_branch);

        if (st->u.if_s.else_branch != NULL) {
            Stmt *els = st->u.if_s.else_branch;
            /* "else if" сворачивается в elif. */
            if (els->kind == ST_IF) {
                line_start(g, els->pos.line);
                sb_puts(&g->out, "elif ");
                gen_condition(g, els->u.if_s.cond, PREC_TERNARY);
                sb_puts(&g->out, ":\n");
                gen_suite(g, els->u.if_s.then_branch);
                if (els->u.if_s.else_branch != NULL) {
                    Stmt outer;
                    /* Хвост цепочки обрабатывается тем же кодом. */
                    outer = *els;
                    st = &outer;
                    while (st->u.if_s.else_branch != NULL &&
                           st->u.if_s.else_branch->kind == ST_IF) {
                        Stmt *next = st->u.if_s.else_branch;
                        line_start(g, next->pos.line);
                        sb_puts(&g->out, "elif ");
                        gen_condition(g, next->u.if_s.cond, PREC_TERNARY);
                        sb_puts(&g->out, ":\n");
                        gen_suite(g, next->u.if_s.then_branch);
                        st = next;
                    }
                    if (st->u.if_s.else_branch != NULL) {
                        line_start(g, st->u.if_s.else_branch->pos.line);
                        sb_puts(&g->out, "else:\n");
                        gen_suite(g, st->u.if_s.else_branch);
                    }
                }
            } else {
                line_start(g, els->pos.line);
                sb_puts(&g->out, "else:\n");
                gen_suite(g, els);
            }
        }
        break;

    case ST_WHILE:
        line_start(g, st->pos.line);
        sb_puts(&g->out, "while ");
        gen_condition(g, st->u.while_s.cond, PREC_TERNARY);
        sb_puts(&g->out, ":\n");
        saved_step = g->pending_step;
        saved_switch = g->in_switch;
        g->pending_step = NULL;
        g->in_switch = 0;
        gen_suite(g, st->u.while_s.body);
        g->pending_step = saved_step;
        g->in_switch = saved_switch;
        break;

    case ST_DO:
        /* В Python нет do-while: тело выполняется в бесконечном цикле,
         * а условие проверяется в конце. */
        line_start(g, st->pos.line);
        sb_puts(&g->out, "while True:\n");
        saved_step = g->pending_step;
        saved_switch = g->in_switch;
        g->pending_step = NULL;
        g->in_switch = 0;
        g->indent++;
        if (st->u.do_s.body != NULL) gen_stmt(g, st->u.do_s.body);
        indent_line(g);
        sb_puts(&g->out, "if not (");
        gen_condition(g, st->u.do_s.cond, PREC_TERNARY);
        sb_puts(&g->out, "):\n");
        g->indent++;
        indent_line(g);
        sb_puts(&g->out, "break\n");
        g->indent -= 2;
        g->pending_step = saved_step;
        g->in_switch = saved_switch;
        break;

    case ST_FOR:
        /* Отступ ставит сам gen_for: в форме while первой идёт
         * инициализация, которая выводится как отдельная инструкция. */
        flush_comments_before(g, st->pos.line);
        gen_for(g, st);
        break;

    case ST_SWITCH:
        line_start(g, st->pos.line);
        gen_switch(g, st);
        break;

    case ST_BREAK:
        /* break, относящийся к switch, не нужен: ветвь if и так
         * заканчивается. break цикла сохраняется. */
        if (!g->in_switch) {
            line_start(g, st->pos.line);
            sb_puts(&g->out, "break\n");
            attach_trailing_comment(g, st->pos.line);
        }
        break;

    case ST_CONTINUE:
        line_start(g, st->pos.line);
        /* В цикле for, развёрнутом в while, шаг обязан выполниться
         * и перед continue — иначе цикл не завершится. */
        if (g->pending_step != NULL) {
            gen_expr_stmt(g, g->pending_step, st->pos.line);
            indent_line(g);
        }
        sb_puts(&g->out, "continue\n");
        attach_trailing_comment(g, st->pos.line);
        break;

    case ST_RETURN:
        line_start(g, st->pos.line);
        if (st->u.expr == NULL) {
            sb_puts(&g->out, "return\n");
        } else {
            sb_puts(&g->out, "return ");
            gen_expr(g, st->u.expr, PREC_TERNARY);
            sb_putc(&g->out, '\n');
        }
        attach_trailing_comment(g, st->pos.line);
        break;
    }
}

/* --- функции --- */

/* Собирает глобальные переменные, которым функция присваивает
 * значение: в Python это требует объявления global. */
static void gen_global_declarations(Gen *g, Function *fn)
{
    Symbol *sym;
    int first = 1;

    for (sym = symtab_globals(g->tab); sym != NULL; sym = sym->scope_next) {
        if (sym->kind == SYM_FUNC || !sym->is_global) continue;
        if (!stmt_assigns_symbol(fn->body, sym)) continue;
        if (first) {
            indent_line(g);
            sb_puts(&g->out, "global ");
            first = 0;
        } else {
            sb_puts(&g->out, ", ");
        }
        sb_puts(&g->out, sym->py_name);
    }
    if (!first) sb_putc(&g->out, '\n');
}

static void gen_function(Gen *g, Function *fn)
{
    Symbol *sym = symtab_lookup(g->tab, fn->name);
    int i;
    size_t before;

    if (fn->body == NULL) return; /* прототип в Python не нужен */

    flush_comments_before(g, fn->pos.line);

    sb_printf(&g->out, "def %s(", py_name_of(sym, fn->name));
    for (i = 0; i < fn->nparams; i++) {
        if (i > 0) sb_puts(&g->out, ", ");
        sb_puts(&g->out, py_name_of(fn->params[i].sym, fn->params[i].name));
    }
    sb_puts(&g->out, "):\n");

    g->indent++;
    gen_global_declarations(g, fn);
    before = sb_len(&g->out);
    for (i = 0; i < fn->body->u.block.count; i++)
        gen_stmt(g, fn->body->u.block.items[i]);
    if (sb_len(&g->out) == before) {
        indent_line(g);
        sb_puts(&g->out, "pass\n");
    }
    g->indent--;
    sb_putc(&g->out, '\n');
}

/* --- служебная часть --- */

static void gen_prelude(Gen *g, StrBuf *dst)
{
    int need_blank = 0;

    sb_puts(dst, "# Получено транслятором c2py из исходного текста на языке C.\n\n");

    if (g->need_sys || g->need_input) {
        sb_puts(dst, "import sys\n");
        need_blank = 1;
    }
    if (g->need_math) {
        sb_puts(dst, "import math\n");
        need_blank = 1;
    }
    if (g->need_random) {
        sb_puts(dst, "import random\n");
        need_blank = 1;
    }
    if (need_blank) sb_putc(dst, '\n');

    if (g->need_div) {
        sb_puts(dst,
                "def _c_div(a, b):\n"
                "    \"\"\"Целочисленное деление C: усечение к нулю.\n"
                "\n"
                "    Оператор // в Python округляет вниз, поэтому -7 // 2\n"
                "    даёт -4, тогда как в C -7 / 2 равно -3.\n"
                "    \"\"\"\n"
                "    quotient = abs(a) // abs(b)\n"
                "    return -quotient if (a < 0) != (b < 0) else quotient\n"
                "\n\n");
    }
    if (g->need_mod) {
        sb_puts(dst,
                "def _c_mod(a, b):\n"
                "    \"\"\"Остаток от деления в C: знак совпадает с делимым.\"\"\"\n"
                "    return a - _c_div(a, b) * b\n"
                "\n\n");
    }
    if (g->need_input) {
        sb_puts(dst,
                "_c_tokens = []\n"
                "\n"
                "\n"
                "def _c_input():\n"
                "    \"\"\"Очередная лексема стандартного ввода — как это\n"
                "    делает scanf, не обращая внимания на переводы строк.\n"
                "    \"\"\"\n"
                "    while not _c_tokens:\n"
                "        line = sys.stdin.readline()\n"
                "        if not line:\n"
                "            return \"\"\n"
                "        _c_tokens.extend(line.split())\n"
                "    return _c_tokens.pop(0)\n"
                "\n\n");
    }
}

char *codegen_program(Program *prog, SymTab *tab)
{
    Gen g;
    StrBuf final;
    int i;

    memset(&g, 0, sizeof(g));
    sb_init(&g.out);
    g.tab = tab;
    g.indent = 0;
    g.comment_index = 0;

    /* Глобальные переменные и функции выводятся в исходном порядке. */
    for (i = 0; i < prog->count; i++) {
        if (prog->items[i].kind == TL_VAR) {
            Stmt *decl = prog->items[i].u.var_decl;
            line_start(&g, decl->pos.line);
            gen_declaration(&g, decl);
        } else {
            gen_function(&g, prog->items[i].u.func);
        }
    }

    /* Комментарии, оставшиеся после последней инструкции. */
    flush_comments_before(&g, 0);

    /* Точка входа: код возврата main становится кодом возврата процесса. */
    g.need_sys = 1;
    sb_puts(&g.out, "\nif __name__ == \"__main__\":\n    sys.exit(main())\n");

    sb_init(&final);
    gen_prelude(&g, &final);
    sb_puts(&final, sb_cstr(&g.out));
    sb_free(&g.out);
    return sb_detach(&final);
}
