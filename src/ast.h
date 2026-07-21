/* src/ast.h — абстрактное синтаксическое дерево.
 *
 * Синтаксический анализатор не печатает результат, а строит дерево.
 * Это принципиально: между разбором и генерацией появляется место для
 * семантического анализа, а генератор видит конструкцию целиком и может
 * принимать решения, недоступные при построчном выводе (например,
 * свести цикл for к range() только если это действительно допустимо).
 */
#ifndef C2PY_AST_H
#define C2PY_AST_H

#include <stddef.h>

#include "srcpos.h"
#include "types.h"

/* --- операции --- */

typedef enum {
    OP_NONE = 0, /* простое присваивание "=" */
    /* бинарные */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_LT, OP_GT, OP_LE, OP_GE, OP_EQ, OP_NE,
    OP_LAND, OP_LOR,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    /* унарные */
    OP_NEG, OP_PLUS, OP_NOT, OP_BNOT,
    OP_ADDR /* унарный & : допустим только в аргументах scanf */
} Op;

const char *op_spelling(Op op);

/* --- выражения --- */

typedef enum {
    EX_INT_LIT,
    EX_FLOAT_LIT,
    EX_CHAR_LIT,
    EX_STR_LIT,
    EX_IDENT,
    EX_BINARY,
    EX_UNARY,
    EX_ASSIGN,
    EX_COND,    /* тернарный оператор ?: */
    EX_CALL,
    EX_INDEX,   /* a[i] */
    EX_PREINC, EX_PREDEC, EX_POSTINC, EX_POSTDEC,
    EX_CAST,
    EX_COMMA,   /* оператор "запятая": нужен в заголовке for */
    EX_SIZEOF
} ExprKind;

typedef struct Expr Expr;
typedef struct Symbol Symbol;

struct Expr {
    ExprKind kind;
    SrcPos pos;
    const Type *type; /* проставляется семантическим анализом */
    union {
        long long ival; /* EX_INT_LIT, EX_CHAR_LIT */
        double fval;    /* EX_FLOAT_LIT */
        struct {
            char *data; /* раскодированные байты строки */
            int len;
        } str;
        struct {
            char *name;
            Symbol *sym; /* связывается семантическим анализом */
        } ident;
        struct { Op op; Expr *lhs, *rhs; } bin;
        struct { Op op; Expr *operand; } un;
        struct { Op op; Expr *target, *value; } assign; /* op==OP_NONE => "=" */
        struct { Expr *cond, *then_expr, *else_expr; } cond;
        struct {
            char *name;
            Expr **args;
            int nargs, cap;
        } call;
        struct { Expr *base, *index; } index;
        struct { Expr *operand; } incdec;
        struct { const Type *to; Expr *operand; } cast;
        struct {
            Expr **items;
            int count, cap;
        } comma;
        /* sizeof: либо от типа, либо от выражения */
        struct { const Type *of_type; Expr *operand; } size_of;
    } u;
};

Expr *expr_int_lit(long long value, SrcPos pos);
Expr *expr_float_lit(double value, SrcPos pos);
Expr *expr_char_lit(long long value, SrcPos pos);
Expr *expr_str_lit(const char *data, int len, SrcPos pos);
Expr *expr_ident(const char *name, SrcPos pos);
Expr *expr_binary(Op op, Expr *lhs, Expr *rhs, SrcPos pos);
Expr *expr_unary(Op op, Expr *operand, SrcPos pos);
Expr *expr_assign(Op op, Expr *target, Expr *value, SrcPos pos);
Expr *expr_cond(Expr *cond, Expr *then_expr, Expr *else_expr, SrcPos pos);
Expr *expr_call(const char *name, SrcPos pos);
void  expr_call_add_arg(Expr *call, Expr *arg);
Expr *expr_index(Expr *base, Expr *index, SrcPos pos);
Expr *expr_incdec(ExprKind kind, Expr *operand, SrcPos pos);
Expr *expr_cast(const Type *to, Expr *operand, SrcPos pos);
Expr *expr_comma(SrcPos pos);
void  expr_comma_add(Expr *comma, Expr *item);
Expr *expr_sizeof_type(const Type *of_type, SrcPos pos);
Expr *expr_sizeof_expr(Expr *operand, SrcPos pos);
void  expr_free(Expr *e);

/* --- объявления переменных --- */

typedef struct Declarator {
    char *name;
    const Type *type;
    Expr *init;       /* скалярный инициализатор либо NULL */
    Expr **init_list; /* списочный инициализатор {1, 2, 3} */
    int ninit, init_cap;
    int has_init_list;
    SrcPos pos;
    Symbol *sym;
} Declarator;

Declarator *declarator_new(const char *name, const Type *type, Expr *init,
                           SrcPos pos);
void declarator_add_init(Declarator *d, Expr *value);
void declarator_free(Declarator *d);

/* --- инструкции --- */

typedef enum {
    ST_EMPTY,
    ST_EXPR,
    ST_DECL,
    ST_BLOCK,
    ST_IF,
    ST_WHILE,
    ST_DO,
    ST_FOR,
    ST_SWITCH,
    ST_BREAK,
    ST_CONTINUE,
    ST_RETURN
} StmtKind;

typedef struct Stmt Stmt;

typedef struct {
    Stmt **items;
    int count, cap;
} StmtList;

void stmt_list_init(StmtList *list);
void stmt_list_add(StmtList *list, Stmt *stmt);
void stmt_list_free(StmtList *list);

/* Ветвь switch: набор меток case плюс относящиеся к ним инструкции. */
typedef struct SwitchCase {
    Expr **labels;
    int nlabels, label_cap;
    int is_default;
    StmtList body;
    SrcPos pos;
    struct SwitchCase *next;
} SwitchCase;

struct Stmt {
    StmtKind kind;
    SrcPos pos;
    union {
        Expr *expr; /* ST_EXPR, ST_RETURN (может быть NULL) */
        struct {
            Declarator **items;
            int count, cap;
        } decl;
        StmtList block;
        struct { Expr *cond; Stmt *then_branch, *else_branch; } if_s;
        struct { Expr *cond; Stmt *body; } while_s;
        struct { Stmt *body; Expr *cond; } do_s;
        struct { Stmt *init; Expr *cond; Expr *step; Stmt *body; } for_s;
        struct { Expr *subject; SwitchCase *cases; } switch_s;
    } u;
};

Stmt *stmt_empty(SrcPos pos);
Stmt *stmt_expr(Expr *e, SrcPos pos);
Stmt *stmt_decl(SrcPos pos);
void  stmt_decl_add(Stmt *stmt, Declarator *d);
Stmt *stmt_block(SrcPos pos);
void  stmt_block_add(Stmt *block, Stmt *inner);
Stmt *stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, SrcPos pos);
Stmt *stmt_while(Expr *cond, Stmt *body, SrcPos pos);
Stmt *stmt_do(Stmt *body, Expr *cond, SrcPos pos);
Stmt *stmt_for(Stmt *init, Expr *cond, Expr *step, Stmt *body, SrcPos pos);
Stmt *stmt_switch(Expr *subject, SrcPos pos);
Stmt *stmt_break(SrcPos pos);
Stmt *stmt_continue(SrcPos pos);
Stmt *stmt_return(Expr *value, SrcPos pos);
void  stmt_free(Stmt *s);

SwitchCase *switch_case_new(int is_default, SrcPos pos);
void switch_case_add_label(SwitchCase *sc, Expr *label);
void switch_add_case(Stmt *switch_stmt, SwitchCase *sc);

/* --- функции --- */

typedef struct {
    char *name;
    const Type *type;
    SrcPos pos;
    Symbol *sym;
} Param;

typedef struct Function {
    char *name;
    const Type *ret_type;
    Param *params;
    int nparams, param_cap;
    Stmt *body; /* NULL для прототипа */
    SrcPos pos;
} Function;

Function *function_new(const char *name, const Type *ret_type, SrcPos pos);
void function_add_param(Function *fn, const char *name, const Type *type,
                        SrcPos pos);
void function_set_body(Function *fn, Stmt *body);
void function_free(Function *fn);

/* --- программа --- */

typedef enum { TL_VAR, TL_FUNC } TopLevelKind;

typedef struct {
    TopLevelKind kind;
    union {
        Stmt *var_decl; /* ST_DECL */
        Function *func;
    } u;
} TopLevel;

typedef struct {
    TopLevel *items;
    int count, cap;
} Program;

Program *program_new(void);
void program_add_var(Program *prog, Stmt *decl);
void program_add_function(Program *prog, Function *fn);
void program_free(Program *prog);

#endif /* C2PY_AST_H */
