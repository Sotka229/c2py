/* src/ast.c — конструирование и освобождение узлов дерева. */
#include "ast.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

const char *op_spelling(Op op)
{
    switch (op) {
    case OP_NONE: return "=";
    case OP_ADD:  return "+";
    case OP_SUB:  return "-";
    case OP_MUL:  return "*";
    case OP_DIV:  return "/";
    case OP_MOD:  return "%";
    case OP_LT:   return "<";
    case OP_GT:   return ">";
    case OP_LE:   return "<=";
    case OP_GE:   return ">=";
    case OP_EQ:   return "==";
    case OP_NE:   return "!=";
    case OP_LAND: return "&&";
    case OP_LOR:  return "||";
    case OP_BAND: return "&";
    case OP_BOR:  return "|";
    case OP_BXOR: return "^";
    case OP_SHL:  return "<<";
    case OP_SHR:  return ">>";
    case OP_NEG:  return "-";
    case OP_PLUS: return "+";
    case OP_NOT:  return "!";
    case OP_BNOT: return "~";
    case OP_ADDR: return "&";
    }
    return "?";
}

/* --- выражения --- */

static Expr *expr_new(ExprKind kind, SrcPos pos)
{
    Expr *e = (Expr *)xcalloc(1, sizeof(Expr));
    e->kind = kind;
    e->pos = pos;
    e->type = NULL;
    return e;
}

Expr *expr_int_lit(long long value, SrcPos pos)
{
    Expr *e = expr_new(EX_INT_LIT, pos);
    e->u.ival = value;
    return e;
}

Expr *expr_char_lit(long long value, SrcPos pos)
{
    Expr *e = expr_new(EX_CHAR_LIT, pos);
    e->u.ival = value;
    return e;
}

Expr *expr_float_lit(double value, SrcPos pos)
{
    Expr *e = expr_new(EX_FLOAT_LIT, pos);
    e->u.fval = value;
    return e;
}

Expr *expr_str_lit(const char *data, int len, SrcPos pos)
{
    Expr *e = expr_new(EX_STR_LIT, pos);
    e->u.str.data = (char *)xmalloc((size_t)len + 1);
    if (len > 0) memcpy(e->u.str.data, data, (size_t)len);
    e->u.str.data[len] = '\0';
    e->u.str.len = len;
    return e;
}

Expr *expr_ident(const char *name, SrcPos pos)
{
    Expr *e = expr_new(EX_IDENT, pos);
    e->u.ident.name = xstrdup(name);
    e->u.ident.sym = NULL;
    return e;
}

Expr *expr_binary(Op op, Expr *lhs, Expr *rhs, SrcPos pos)
{
    Expr *e = expr_new(EX_BINARY, pos);
    e->u.bin.op = op;
    e->u.bin.lhs = lhs;
    e->u.bin.rhs = rhs;
    return e;
}

Expr *expr_unary(Op op, Expr *operand, SrcPos pos)
{
    Expr *e = expr_new(EX_UNARY, pos);
    e->u.un.op = op;
    e->u.un.operand = operand;
    return e;
}

Expr *expr_assign(Op op, Expr *target, Expr *value, SrcPos pos)
{
    Expr *e = expr_new(EX_ASSIGN, pos);
    e->u.assign.op = op;
    e->u.assign.target = target;
    e->u.assign.value = value;
    return e;
}

Expr *expr_cond(Expr *cond, Expr *then_expr, Expr *else_expr, SrcPos pos)
{
    Expr *e = expr_new(EX_COND, pos);
    e->u.cond.cond = cond;
    e->u.cond.then_expr = then_expr;
    e->u.cond.else_expr = else_expr;
    return e;
}

Expr *expr_call(const char *name, SrcPos pos)
{
    Expr *e = expr_new(EX_CALL, pos);
    e->u.call.name = xstrdup(name);
    e->u.call.args = NULL;
    e->u.call.nargs = 0;
    e->u.call.cap = 0;
    return e;
}

void expr_call_add_arg(Expr *call, Expr *arg)
{
    if (call->u.call.nargs == call->u.call.cap) {
        call->u.call.cap = call->u.call.cap ? call->u.call.cap * 2 : 4;
        call->u.call.args = (Expr **)xrealloc(
            call->u.call.args, (size_t)call->u.call.cap * sizeof(Expr *));
    }
    call->u.call.args[call->u.call.nargs++] = arg;
}

Expr *expr_index(Expr *base, Expr *index, SrcPos pos)
{
    Expr *e = expr_new(EX_INDEX, pos);
    e->u.index.base = base;
    e->u.index.index = index;
    return e;
}

Expr *expr_incdec(ExprKind kind, Expr *operand, SrcPos pos)
{
    Expr *e = expr_new(kind, pos);
    e->u.incdec.operand = operand;
    return e;
}

Expr *expr_cast(const Type *to, Expr *operand, SrcPos pos)
{
    Expr *e = expr_new(EX_CAST, pos);
    e->u.cast.to = to;
    e->u.cast.operand = operand;
    return e;
}

Expr *expr_comma(SrcPos pos) { return expr_new(EX_COMMA, pos); }

void expr_comma_add(Expr *comma, Expr *item)
{
    if (comma->u.comma.count == comma->u.comma.cap) {
        comma->u.comma.cap = comma->u.comma.cap ? comma->u.comma.cap * 2 : 4;
        comma->u.comma.items = (Expr **)xrealloc(
            comma->u.comma.items, (size_t)comma->u.comma.cap * sizeof(Expr *));
    }
    comma->u.comma.items[comma->u.comma.count++] = item;
}

Expr *expr_sizeof_type(const Type *of_type, SrcPos pos)
{
    Expr *e = expr_new(EX_SIZEOF, pos);
    e->u.size_of.of_type = of_type;
    e->u.size_of.operand = NULL;
    return e;
}

Expr *expr_sizeof_expr(Expr *operand, SrcPos pos)
{
    Expr *e = expr_new(EX_SIZEOF, pos);
    e->u.size_of.of_type = NULL;
    e->u.size_of.operand = operand;
    return e;
}

void expr_free(Expr *e)
{
    int i;
    if (e == NULL) return;

    switch (e->kind) {
    case EX_STR_LIT:
        free(e->u.str.data);
        break;
    case EX_IDENT:
        free(e->u.ident.name);
        break;
    case EX_BINARY:
        expr_free(e->u.bin.lhs);
        expr_free(e->u.bin.rhs);
        break;
    case EX_UNARY:
        expr_free(e->u.un.operand);
        break;
    case EX_ASSIGN:
        expr_free(e->u.assign.target);
        expr_free(e->u.assign.value);
        break;
    case EX_COND:
        expr_free(e->u.cond.cond);
        expr_free(e->u.cond.then_expr);
        expr_free(e->u.cond.else_expr);
        break;
    case EX_CALL:
        for (i = 0; i < e->u.call.nargs; i++) expr_free(e->u.call.args[i]);
        free(e->u.call.args);
        free(e->u.call.name);
        break;
    case EX_INDEX:
        expr_free(e->u.index.base);
        expr_free(e->u.index.index);
        break;
    case EX_PREINC:
    case EX_PREDEC:
    case EX_POSTINC:
    case EX_POSTDEC:
        expr_free(e->u.incdec.operand);
        break;
    case EX_CAST:
        expr_free(e->u.cast.operand);
        break;
    case EX_COMMA:
        for (i = 0; i < e->u.comma.count; i++) expr_free(e->u.comma.items[i]);
        free(e->u.comma.items);
        break;
    case EX_SIZEOF:
        expr_free(e->u.size_of.operand);
        break;
    case EX_INT_LIT:
    case EX_FLOAT_LIT:
    case EX_CHAR_LIT:
        break;
    }
    free(e);
}

/* --- объявления --- */

Declarator *declarator_new(const char *name, const Type *type, Expr *init,
                           SrcPos pos)
{
    Declarator *d = (Declarator *)xcalloc(1, sizeof(Declarator));
    d->name = xstrdup(name);
    d->type = type;
    d->init = init;
    d->pos = pos;
    return d;
}

void declarator_add_init(Declarator *d, Expr *value)
{
    if (d->ninit == d->init_cap) {
        d->init_cap = d->init_cap ? d->init_cap * 2 : 4;
        d->init_list =
            (Expr **)xrealloc(d->init_list, (size_t)d->init_cap * sizeof(Expr *));
    }
    d->init_list[d->ninit++] = value;
    d->has_init_list = 1;
}

void declarator_free(Declarator *d)
{
    int i;
    if (d == NULL) return;
    free(d->name);
    expr_free(d->init);
    for (i = 0; i < d->ninit; i++) expr_free(d->init_list[i]);
    free(d->init_list);
    free(d);
}

/* --- списки инструкций --- */

void stmt_list_init(StmtList *list)
{
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void stmt_list_add(StmtList *list, Stmt *stmt)
{
    if (stmt == NULL) return;
    if (list->count == list->cap) {
        list->cap = list->cap ? list->cap * 2 : 8;
        list->items =
            (Stmt **)xrealloc(list->items, (size_t)list->cap * sizeof(Stmt *));
    }
    list->items[list->count++] = stmt;
}

void stmt_list_free(StmtList *list)
{
    int i;
    for (i = 0; i < list->count; i++) stmt_free(list->items[i]);
    free(list->items);
    stmt_list_init(list);
}

/* --- инструкции --- */

static Stmt *stmt_new(StmtKind kind, SrcPos pos)
{
    Stmt *s = (Stmt *)xcalloc(1, sizeof(Stmt));
    s->kind = kind;
    s->pos = pos;
    return s;
}

Stmt *stmt_empty(SrcPos pos) { return stmt_new(ST_EMPTY, pos); }

Stmt *stmt_expr(Expr *e, SrcPos pos)
{
    Stmt *s = stmt_new(ST_EXPR, pos);
    s->u.expr = e;
    return s;
}

Stmt *stmt_decl(SrcPos pos) { return stmt_new(ST_DECL, pos); }

void stmt_decl_add(Stmt *stmt, Declarator *d)
{
    if (stmt->u.decl.count == stmt->u.decl.cap) {
        stmt->u.decl.cap = stmt->u.decl.cap ? stmt->u.decl.cap * 2 : 4;
        stmt->u.decl.items = (Declarator **)xrealloc(
            stmt->u.decl.items, (size_t)stmt->u.decl.cap * sizeof(Declarator *));
    }
    stmt->u.decl.items[stmt->u.decl.count++] = d;
}

Stmt *stmt_block(SrcPos pos)
{
    Stmt *s = stmt_new(ST_BLOCK, pos);
    stmt_list_init(&s->u.block);
    return s;
}

void stmt_block_add(Stmt *block, Stmt *inner)
{
    stmt_list_add(&block->u.block, inner);
}

Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, SrcPos pos)
{
    Stmt *s = stmt_new(ST_IF, pos);
    s->u.if_s.cond = cond;
    s->u.if_s.then_branch = then_branch;
    s->u.if_s.else_branch = else_branch;
    return s;
}

Stmt *stmt_while(Expr *cond, Stmt *body, SrcPos pos)
{
    Stmt *s = stmt_new(ST_WHILE, pos);
    s->u.while_s.cond = cond;
    s->u.while_s.body = body;
    return s;
}

Stmt *stmt_do(Stmt *body, Expr *cond, SrcPos pos)
{
    Stmt *s = stmt_new(ST_DO, pos);
    s->u.do_s.body = body;
    s->u.do_s.cond = cond;
    return s;
}

Stmt *stmt_for(Stmt *init, Expr *cond, Expr *step, Stmt *body, SrcPos pos)
{
    Stmt *s = stmt_new(ST_FOR, pos);
    s->u.for_s.init = init;
    s->u.for_s.cond = cond;
    s->u.for_s.step = step;
    s->u.for_s.body = body;
    return s;
}

Stmt *stmt_switch(Expr *subject, SrcPos pos)
{
    Stmt *s = stmt_new(ST_SWITCH, pos);
    s->u.switch_s.subject = subject;
    s->u.switch_s.cases = NULL;
    return s;
}

Stmt *stmt_break(SrcPos pos) { return stmt_new(ST_BREAK, pos); }
Stmt *stmt_continue(SrcPos pos) { return stmt_new(ST_CONTINUE, pos); }

Stmt *stmt_return(Expr *value, SrcPos pos)
{
    Stmt *s = stmt_new(ST_RETURN, pos);
    s->u.expr = value;
    return s;
}

SwitchCase *switch_case_new(int is_default, SrcPos pos)
{
    SwitchCase *sc = (SwitchCase *)xcalloc(1, sizeof(SwitchCase));
    sc->is_default = is_default;
    sc->pos = pos;
    stmt_list_init(&sc->body);
    return sc;
}

void switch_case_add_label(SwitchCase *sc, Expr *label)
{
    if (sc->nlabels == sc->label_cap) {
        sc->label_cap = sc->label_cap ? sc->label_cap * 2 : 4;
        sc->labels =
            (Expr **)xrealloc(sc->labels, (size_t)sc->label_cap * sizeof(Expr *));
    }
    sc->labels[sc->nlabels++] = label;
}

/* Ветви добавляются в конец, чтобы сохранить исходный порядок. */
void switch_add_case(Stmt *switch_stmt, SwitchCase *sc)
{
    SwitchCase **slot = &switch_stmt->u.switch_s.cases;
    while (*slot != NULL) slot = &(*slot)->next;
    *slot = sc;
}

static void switch_cases_free(SwitchCase *sc)
{
    while (sc != NULL) {
        SwitchCase *next = sc->next;
        int i;
        for (i = 0; i < sc->nlabels; i++) expr_free(sc->labels[i]);
        free(sc->labels);
        stmt_list_free(&sc->body);
        free(sc);
        sc = next;
    }
}

void stmt_free(Stmt *s)
{
    int i;
    if (s == NULL) return;

    switch (s->kind) {
    case ST_EXPR:
    case ST_RETURN:
        expr_free(s->u.expr);
        break;
    case ST_DECL:
        for (i = 0; i < s->u.decl.count; i++)
            declarator_free(s->u.decl.items[i]);
        free(s->u.decl.items);
        break;
    case ST_BLOCK:
        stmt_list_free(&s->u.block);
        break;
    case ST_IF:
        expr_free(s->u.if_s.cond);
        stmt_free(s->u.if_s.then_branch);
        stmt_free(s->u.if_s.else_branch);
        break;
    case ST_WHILE:
        expr_free(s->u.while_s.cond);
        stmt_free(s->u.while_s.body);
        break;
    case ST_DO:
        stmt_free(s->u.do_s.body);
        expr_free(s->u.do_s.cond);
        break;
    case ST_FOR:
        stmt_free(s->u.for_s.init);
        expr_free(s->u.for_s.cond);
        expr_free(s->u.for_s.step);
        stmt_free(s->u.for_s.body);
        break;
    case ST_SWITCH:
        expr_free(s->u.switch_s.subject);
        switch_cases_free(s->u.switch_s.cases);
        break;
    case ST_EMPTY:
    case ST_BREAK:
    case ST_CONTINUE:
        break;
    }
    free(s);
}

/* --- функции --- */

Function *function_new(const char *name, const Type *ret_type, SrcPos pos)
{
    Function *fn = (Function *)xcalloc(1, sizeof(Function));
    fn->name = xstrdup(name);
    fn->ret_type = ret_type;
    fn->pos = pos;
    return fn;
}

void function_add_param(Function *fn, const char *name, const Type *type,
                        SrcPos pos)
{
    if (fn->nparams == fn->param_cap) {
        fn->param_cap = fn->param_cap ? fn->param_cap * 2 : 4;
        fn->params =
            (Param *)xrealloc(fn->params, (size_t)fn->param_cap * sizeof(Param));
    }
    fn->params[fn->nparams].name = xstrdup(name);
    fn->params[fn->nparams].type = type;
    fn->params[fn->nparams].pos = pos;
    fn->params[fn->nparams].sym = NULL;
    fn->nparams++;
}

void function_set_body(Function *fn, Stmt *body) { fn->body = body; }

void function_free(Function *fn)
{
    int i;
    if (fn == NULL) return;
    for (i = 0; i < fn->nparams; i++) free(fn->params[i].name);
    free(fn->params);
    free(fn->name);
    stmt_free(fn->body);
    free(fn);
}

/* --- программа --- */

Program *program_new(void)
{
    return (Program *)xcalloc(1, sizeof(Program));
}

static TopLevel *program_slot(Program *prog)
{
    if (prog->count == prog->cap) {
        prog->cap = prog->cap ? prog->cap * 2 : 8;
        prog->items = (TopLevel *)xrealloc(prog->items,
                                           (size_t)prog->cap * sizeof(TopLevel));
    }
    return &prog->items[prog->count++];
}

void program_add_var(Program *prog, Stmt *decl)
{
    TopLevel *tl = program_slot(prog);
    tl->kind = TL_VAR;
    tl->u.var_decl = decl;
}

void program_add_function(Program *prog, Function *fn)
{
    TopLevel *tl = program_slot(prog);
    tl->kind = TL_FUNC;
    tl->u.func = fn;
}

void program_free(Program *prog)
{
    int i;
    if (prog == NULL) return;
    for (i = 0; i < prog->count; i++) {
        if (prog->items[i].kind == TL_VAR)
            stmt_free(prog->items[i].u.var_decl);
        else
            function_free(prog->items[i].u.func);
    }
    free(prog->items);
    free(prog);
}
