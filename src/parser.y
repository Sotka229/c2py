/* src/parser.y — синтаксический анализатор подмножества языка C.
 *
 * Анализатор строит абстрактное синтаксическое дерево и ничего не печатает.
 * Реализована полная лестница приоритетов операций языка C с правильной
 * ассоциативностью, а также восстановление после ошибок, позволяющее
 * сообщить о нескольких проблемах за один запуск.
 */
/* Вспомогательный код размещается через "%code", а не через "%{ %}":
 * bison выводит блок "%{ %}" раньше определения YYLTYPE, и функции,
 * принимающие позицию, там ещё не могут быть объявлены. */
%code {
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "comments.h"
#include "diag.h"
#include "lexer.h"
#include "parse.h"
#include "util.h"

int yylex(void);
void yyerror(const char *msg);

/* Строящаяся программа. */
static Program *cur_program = NULL;

/* Базовый тип текущего объявления: устанавливается при свёртке
 * спецификаторов и читается правилами описателей. Так "int a, b[3]"
 * получает типы int и int[3] соответственно. */
static const Type *cur_base_type = NULL;

/* Функция, для которой сейчас разбирается список параметров. */
static Function *cur_function = NULL;

/* Собираемая инструкция объявления: "int a, b = 1;" — это один узел
 * с несколькими описателями. */
static Stmt *cur_declaration = NULL;

static SrcPos loc_pos(YYLTYPE loc)
{
    SrcPos p;
    p.line = loc.first_line;
    p.col = loc.first_column;
    return p;
}

/* Объединяет спецификаторы типа: "long int" -> long, "unsigned int" -> int. */
static const Type *combine_specifiers(const Type *a, const Type *b)
{
    if (a == NULL) return b;
    if (b == NULL) return a;
    /* Более конкретный спецификатор побеждает обобщённый int. */
    if (a == type_int()) return b;
    if (b == type_int()) return a;
    return a;
}

/* Список инструкций размещается в куче, пока собирается правилами. */
static StmtList *list_new(void)
{
    StmtList *l = (StmtList *)xmalloc(sizeof(StmtList));
    stmt_list_init(l);
    return l;
}

/* Превращает собранный список в блок, передавая владение элементами. */
static Stmt *list_to_block(StmtList *l, SrcPos pos)
{
    Stmt *b = stmt_block(pos);
    if (l != NULL) {
        b->u.block = *l;
        free(l);
    }
    return b;
}

/* Узел "запятая" используется как временный контейнер для списка
 * инициализаторов: у него уже есть нужная структура хранения. */
static void take_init_list(Declarator *d, Expr *carrier)
{
    int i;
    for (i = 0; i < carrier->u.comma.count; i++)
        declarator_add_init(d, carrier->u.comma.items[i]);
    carrier->u.comma.count = 0; /* элементы переданы описателю */
    expr_free(carrier);
}

/* Вызов возможен только по имени функции: указателей на функции
 * в поддерживаемом подмножестве нет. */
static Expr *make_call(Expr *callee, Expr *args, YYLTYPE loc)
{
    Expr *call;
    int i;

    if (callee->kind != EX_IDENT) {
        diag_error_at(loc_pos(loc),
                      "only a direct function call by name is supported");
        expr_free(callee);
        expr_free(args);
        return expr_int_lit(0, loc_pos(loc));
    }

    call = expr_call(callee->u.ident.name, loc_pos(loc));
    expr_free(callee);

    if (args != NULL) {
        for (i = 0; i < args->u.comma.count; i++)
            expr_call_add_arg(call, args->u.comma.items[i]);
        args->u.comma.count = 0;
        expr_free(args);
    }
    return call;
}
}

%code requires {
#include "ast.h"

/* Значение строкового литерала: байты уже раскодированы,
 * поэтому длина хранится явно и внутри допустим нулевой байт. */
typedef struct {
    char *data;
    int len;
} StrLit;

/* Тип позиции объявляем сами: иначе он становится известен только
 * после пролога, а вспомогательным функциям пролога он уже нужен. */
typedef struct YYLTYPE {
    int first_line;
    int first_column;
    int last_line;
    int last_column;
} YYLTYPE;
#define YYLTYPE_IS_DECLARED 1
#define YYLTYPE_IS_TRIVIAL 1
}

%locations
%error-verbose

/* Ровно два конфликта сдвига/свёртки, оба — в разборе меток switch.
 * Увидев "case 1: case 2:", анализатор может либо продолжить список
 * меток текущей ветви (сдвиг), либо закрыть ветвь с пустым телом и
 * начать новую (свёртка пустого block_item_list_opt). Два конфликта
 * соответствуют двум символам предпросмотра: KW_CASE и KW_DEFAULT.
 *
 * Оба разрешаются сдвигом, то есть подряд идущие метки объединяются
 * в одну ветвь, — именно это и требуется.
 *
 * Указание точного числа превращает любой новый конфликт в ошибку сборки. */
%expect 2

%union {
    long long   ival;
    double      fval;
    char       *sval;
    StrLit      str;
    const Type *type;
    Expr       *expr;
    Stmt       *stmt;
    StmtList   *slist;
    Declarator *declr;
    Function   *func;
    SwitchCase *swcase;
    Op          op;
}

/* --- лексемы со значением --- */
%token <sval> IDENT
%token <ival> INT_LIT
%token <ival> CHAR_LIT
%token <fval> FLOAT_LIT
%token <str>  STRING_LIT

/* --- ключевые слова --- */
%token KW_INT KW_CHAR KW_FLOAT KW_DOUBLE KW_VOID KW_LONG KW_SHORT
%token KW_UNSIGNED KW_SIGNED KW_CONST KW_SIZEOF
%token KW_IF KW_ELSE KW_WHILE KW_DO KW_FOR
%token KW_SWITCH KW_CASE KW_DEFAULT
%token KW_BREAK KW_CONTINUE KW_RETURN

/* --- составные операции --- */
%token INC DEC LE GE EQ NE AND_OP OR_OP SHL SHR
%token ADD_ASSIGN SUB_ASSIGN MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN
%token AND_ASSIGN OR_ASSIGN XOR_ASSIGN SHL_ASSIGN SHR_ASSIGN

/* Разрешение неоднозначности "висячего else":
 * else всегда относится к ближайшему незакрытому if. */
%nonassoc THEN
%nonassoc KW_ELSE

%type <type>   type_specifier declaration_specifiers type_name
%type <expr>   primary_expr postfix_expr unary_expr cast_expr
%type <expr>   multiplicative_expr additive_expr shift_expr
%type <expr>   relational_expr equality_expr and_expr exclusive_or_expr
%type <expr>   inclusive_or_expr logical_and_expr logical_or_expr
%type <expr>   conditional_expr assignment_expr expr constant_expr
%type <expr>   expr_opt argument_expr_list argument_expr_list_opt
%type <expr>   initializer_list string_literal case_label
%type <op>     assignment_operator unary_operator
%type <stmt>   statement compound_stmt expression_stmt selection_stmt
%type <stmt>   iteration_stmt jump_stmt block_item declaration
%type <stmt>   for_init
%type <slist>  block_item_list block_item_list_opt
%type <declr>  init_declarator declarator
%type <func>   function_header
%type <swcase> switch_section switch_body case_label_list

/* Освобождение значений, отброшенных при восстановлении после ошибки. */
%destructor { free($$); } <sval>
%destructor { free($$.data); } <str>
%destructor { expr_free($$); } <expr>
%destructor { stmt_free($$); } <stmt>
%destructor { declarator_free($$); } <declr>

%start translation_unit

%%

translation_unit
    : /* пустая программа */
    | translation_unit external_declaration
    ;

external_declaration
    : function_header compound_stmt
      {
          function_set_body($1, $2);
          program_add_function(cur_program, $1);
          cur_function = NULL;
      }
    | function_header ';'
      {
          /* прототип: тело отсутствует */
          program_add_function(cur_program, $1);
          cur_function = NULL;
      }
    | declaration
      {
          program_add_var(cur_program, $1);
      }
    | error ';'
      {
          yyerrok;
      }
    ;

function_header
    : declaration_specifiers IDENT '('
      {
          cur_function = function_new($2, $1, loc_pos(@2));
          free($2);
      }
      parameter_list_opt ')'
      {
          $$ = cur_function;
      }
    ;

parameter_list_opt
    : /* пусто */
    | parameter_list
    ;

parameter_list
    : parameter_declaration
    | parameter_list ',' parameter_declaration
    ;

parameter_declaration
    : declaration_specifiers IDENT
      {
          function_add_param(cur_function, $2, $1, loc_pos(@2));
          free($2);
      }
    | declaration_specifiers IDENT '[' ']'
      {
          function_add_param(cur_function, $2,
                             type_array($1, TYPE_LENGTH_UNKNOWN), loc_pos(@2));
          free($2);
      }
    | declaration_specifiers IDENT '[' INT_LIT ']'
      {
          function_add_param(cur_function, $2, type_array($1, (int)$4),
                             loc_pos(@2));
          free($2);
      }
    | declaration_specifiers
      {
          /* Безымянный параметр. Форма "(void)" означает,
           * что параметров нет, и ничего добавлять не нужно. */
          if ($1 != type_void())
              diag_error_at(loc_pos(@1), "parameter name is required");
      }
    ;

/* --- объявления --- */

declaration
    : declaration_specifiers init_declarator_list ';'
      {
          $$ = cur_declaration;
          cur_declaration = NULL;
      }
    ;

declaration_specifiers
    : type_specifier
      { $$ = $1; cur_base_type = $$; }
    | KW_CONST declaration_specifiers
      { $$ = $2; cur_base_type = $$; }
    | type_specifier declaration_specifiers
      { $$ = combine_specifiers($1, $2); cur_base_type = $$; }
    ;

type_specifier
    : KW_VOID     { $$ = type_void(); }
    | KW_CHAR     { $$ = type_char(); }
    | KW_SHORT    { $$ = type_short(); }
    | KW_INT      { $$ = type_int(); }
    | KW_LONG     { $$ = type_long(); }
    | KW_FLOAT    { $$ = type_float(); }
    | KW_DOUBLE   { $$ = type_double(); }
    | KW_UNSIGNED { $$ = type_int(); }
    | KW_SIGNED   { $$ = type_int(); }
    ;

type_name
    : declaration_specifiers { $$ = $1; }
    ;

init_declarator_list
    : init_declarator
      {
          if (cur_declaration == NULL)
              cur_declaration = stmt_decl($1->pos);
          stmt_decl_add(cur_declaration, $1);
      }
    | init_declarator_list ',' init_declarator
      {
          stmt_decl_add(cur_declaration, $3);
      }
    ;

init_declarator
    : declarator
      { $$ = $1; }
    | declarator '=' assignment_expr
      { $1->init = $3; $$ = $1; }
    | declarator '=' '{' initializer_list '}'
      {
          take_init_list($1, $4);
          /* Размер массива может быть задан списком: int a[] = {1,2,3} */
          if (type_is_array($1->type) &&
              type_array_length($1->type) == TYPE_LENGTH_UNKNOWN)
              $1->type = type_array(type_element($1->type), $1->ninit);
          $$ = $1;
      }
    ;

declarator
    : IDENT
      {
          $$ = declarator_new($1, cur_base_type, NULL, loc_pos(@1));
          free($1);
      }
    | IDENT '[' INT_LIT ']'
      {
          $$ = declarator_new($1, type_array(cur_base_type, (int)$3), NULL,
                              loc_pos(@1));
          free($1);
      }
    | IDENT '[' ']'
      {
          $$ = declarator_new($1,
                              type_array(cur_base_type, TYPE_LENGTH_UNKNOWN),
                              NULL, loc_pos(@1));
          free($1);
      }
    ;

initializer_list
    : assignment_expr
      {
          $$ = expr_comma(loc_pos(@1));
          expr_comma_add($$, $1);
      }
    | initializer_list ',' assignment_expr
      {
          expr_comma_add($1, $3);
          $$ = $1;
      }
    ;

/* --- инструкции --- */

compound_stmt
    : '{' block_item_list_opt '}'
      { $$ = list_to_block($2, loc_pos(@1)); }
    ;

block_item_list_opt
    : /* пусто */        { $$ = list_new(); }
    | block_item_list    { $$ = $1; }
    ;

block_item_list
    : block_item
      { $$ = list_new(); stmt_list_add($$, $1); }
    | block_item_list block_item
      { stmt_list_add($1, $2); $$ = $1; }
    ;

block_item
    : declaration  { $$ = $1; }
    | statement    { $$ = $1; }
    ;

statement
    : expression_stmt { $$ = $1; }
    | compound_stmt   { $$ = $1; }
    | selection_stmt  { $$ = $1; }
    | iteration_stmt  { $$ = $1; }
    | jump_stmt       { $$ = $1; }
    | error ';'       { $$ = stmt_empty(loc_pos(@1)); yyerrok; }
    ;

expression_stmt
    : ';'         { $$ = stmt_empty(loc_pos(@1)); }
    | expr ';'    { $$ = stmt_expr($1, loc_pos(@1)); }
    ;

selection_stmt
    : KW_IF '(' expr ')' statement %prec THEN
      { $$ = stmt_if($3, $5, NULL, loc_pos(@1)); }
    | KW_IF '(' expr ')' statement KW_ELSE statement
      { $$ = stmt_if($3, $5, $7, loc_pos(@1)); }
    | KW_SWITCH '(' expr ')' '{' switch_body '}'
      {
          $$ = stmt_switch($3, loc_pos(@1));
          $$->u.switch_s.cases = $6;
      }
    ;

switch_body
    : /* пусто */                 { $$ = NULL; }
    | switch_body switch_section
      {
          if ($1 == NULL) {
              $$ = $2;
          } else {
              SwitchCase *tail = $1;
              while (tail->next != NULL) tail = tail->next;
              tail->next = $2;
              $$ = $1;
          }
      }
    ;

switch_section
    : case_label_list block_item_list_opt
      {
          $$ = $1;
          if ($2 != NULL) {
              $$->body = *$2;
              free($2);
          }
      }
    ;

case_label_list
    : case_label
      {
          $$ = switch_case_new($1 == NULL, loc_pos(@1));
          if ($1 != NULL) switch_case_add_label($$, $1);
      }
    | case_label_list case_label
      {
          $$ = $1;
          if ($2 == NULL) $$->is_default = 1;
          else switch_case_add_label($$, $2);
      }
    ;

case_label
    : KW_CASE constant_expr ':' { $$ = $2; }
    | KW_DEFAULT ':'            { $$ = NULL; }
    ;

iteration_stmt
    : KW_WHILE '(' expr ')' statement
      { $$ = stmt_while($3, $5, loc_pos(@1)); }
    | KW_DO statement KW_WHILE '(' expr ')' ';'
      { $$ = stmt_do($2, $5, loc_pos(@1)); }
    | KW_FOR '(' for_init expr_opt ';' expr_opt ')' statement
      { $$ = stmt_for($3, $4, $6, $8, loc_pos(@1)); }
    ;

/* Первая часть заголовка for: выражение, объявление (C99) или пусто. */
for_init
    : ';'          { $$ = NULL; }
    | expr ';'     { $$ = stmt_expr($1, loc_pos(@1)); }
    | declaration  { $$ = $1; }
    ;

expr_opt
    : /* пусто */  { $$ = NULL; }
    | expr         { $$ = $1; }
    ;

jump_stmt
    : KW_BREAK ';'          { $$ = stmt_break(loc_pos(@1)); }
    | KW_CONTINUE ';'       { $$ = stmt_continue(loc_pos(@1)); }
    | KW_RETURN ';'         { $$ = stmt_return(NULL, loc_pos(@1)); }
    | KW_RETURN expr ';'    { $$ = stmt_return($2, loc_pos(@1)); }
    ;

/* --- выражения: от слабых операций к сильным --- */

expr
    : assignment_expr
      { $$ = $1; }
    | expr ',' assignment_expr
      {
          if ($1->kind == EX_COMMA) {
              expr_comma_add($1, $3);
              $$ = $1;
          } else {
              $$ = expr_comma(loc_pos(@1));
              expr_comma_add($$, $1);
              expr_comma_add($$, $3);
          }
      }
    ;

assignment_expr
    : conditional_expr
      { $$ = $1; }
    | unary_expr assignment_operator assignment_expr
      { $$ = expr_assign($2, $1, $3, loc_pos(@2)); }
    ;

assignment_operator
    : '='        { $$ = OP_NONE; }
    | ADD_ASSIGN { $$ = OP_ADD; }
    | SUB_ASSIGN { $$ = OP_SUB; }
    | MUL_ASSIGN { $$ = OP_MUL; }
    | DIV_ASSIGN { $$ = OP_DIV; }
    | MOD_ASSIGN { $$ = OP_MOD; }
    | AND_ASSIGN { $$ = OP_BAND; }
    | OR_ASSIGN  { $$ = OP_BOR; }
    | XOR_ASSIGN { $$ = OP_BXOR; }
    | SHL_ASSIGN { $$ = OP_SHL; }
    | SHR_ASSIGN { $$ = OP_SHR; }
    ;

constant_expr
    : conditional_expr { $$ = $1; }
    ;

conditional_expr
    : logical_or_expr
      { $$ = $1; }
    | logical_or_expr '?' expr ':' conditional_expr
      { $$ = expr_cond($1, $3, $5, loc_pos(@2)); }
    ;

logical_or_expr
    : logical_and_expr { $$ = $1; }
    | logical_or_expr OR_OP logical_and_expr
      { $$ = expr_binary(OP_LOR, $1, $3, loc_pos(@2)); }
    ;

logical_and_expr
    : inclusive_or_expr { $$ = $1; }
    | logical_and_expr AND_OP inclusive_or_expr
      { $$ = expr_binary(OP_LAND, $1, $3, loc_pos(@2)); }
    ;

inclusive_or_expr
    : exclusive_or_expr { $$ = $1; }
    | inclusive_or_expr '|' exclusive_or_expr
      { $$ = expr_binary(OP_BOR, $1, $3, loc_pos(@2)); }
    ;

exclusive_or_expr
    : and_expr { $$ = $1; }
    | exclusive_or_expr '^' and_expr
      { $$ = expr_binary(OP_BXOR, $1, $3, loc_pos(@2)); }
    ;

and_expr
    : equality_expr { $$ = $1; }
    | and_expr '&' equality_expr
      { $$ = expr_binary(OP_BAND, $1, $3, loc_pos(@2)); }
    ;

equality_expr
    : relational_expr { $$ = $1; }
    | equality_expr EQ relational_expr
      { $$ = expr_binary(OP_EQ, $1, $3, loc_pos(@2)); }
    | equality_expr NE relational_expr
      { $$ = expr_binary(OP_NE, $1, $3, loc_pos(@2)); }
    ;

relational_expr
    : shift_expr { $$ = $1; }
    | relational_expr '<' shift_expr
      { $$ = expr_binary(OP_LT, $1, $3, loc_pos(@2)); }
    | relational_expr '>' shift_expr
      { $$ = expr_binary(OP_GT, $1, $3, loc_pos(@2)); }
    | relational_expr LE shift_expr
      { $$ = expr_binary(OP_LE, $1, $3, loc_pos(@2)); }
    | relational_expr GE shift_expr
      { $$ = expr_binary(OP_GE, $1, $3, loc_pos(@2)); }
    ;

shift_expr
    : additive_expr { $$ = $1; }
    | shift_expr SHL additive_expr
      { $$ = expr_binary(OP_SHL, $1, $3, loc_pos(@2)); }
    | shift_expr SHR additive_expr
      { $$ = expr_binary(OP_SHR, $1, $3, loc_pos(@2)); }
    ;

additive_expr
    : multiplicative_expr { $$ = $1; }
    | additive_expr '+' multiplicative_expr
      { $$ = expr_binary(OP_ADD, $1, $3, loc_pos(@2)); }
    | additive_expr '-' multiplicative_expr
      { $$ = expr_binary(OP_SUB, $1, $3, loc_pos(@2)); }
    ;

multiplicative_expr
    : cast_expr { $$ = $1; }
    | multiplicative_expr '*' cast_expr
      { $$ = expr_binary(OP_MUL, $1, $3, loc_pos(@2)); }
    | multiplicative_expr '/' cast_expr
      { $$ = expr_binary(OP_DIV, $1, $3, loc_pos(@2)); }
    | multiplicative_expr '%' cast_expr
      { $$ = expr_binary(OP_MOD, $1, $3, loc_pos(@2)); }
    ;

cast_expr
    : unary_expr { $$ = $1; }
    | '(' type_name ')' cast_expr
      { $$ = expr_cast($2, $4, loc_pos(@1)); }
    ;

unary_expr
    : postfix_expr { $$ = $1; }
    | INC unary_expr
      { $$ = expr_incdec(EX_PREINC, $2, loc_pos(@1)); }
    | DEC unary_expr
      { $$ = expr_incdec(EX_PREDEC, $2, loc_pos(@1)); }
    | unary_operator cast_expr
      { $$ = expr_unary($1, $2, loc_pos(@1)); }
    | KW_SIZEOF unary_expr
      { $$ = expr_sizeof_expr($2, loc_pos(@1)); }
    | KW_SIZEOF '(' type_name ')'
      { $$ = expr_sizeof_type($3, loc_pos(@1)); }
    ;

unary_operator
    : '+' { $$ = OP_PLUS; }
    | '-' { $$ = OP_NEG; }
    | '!' { $$ = OP_NOT; }
    | '~' { $$ = OP_BNOT; }
    | '&' { $$ = OP_ADDR; }  /* взятие адреса: нужно для scanf */
    ;

postfix_expr
    : primary_expr { $$ = $1; }
    | postfix_expr '[' expr ']'
      { $$ = expr_index($1, $3, loc_pos(@2)); }
    | postfix_expr '(' argument_expr_list_opt ')'
      { $$ = make_call($1, $3, @1); }
    | postfix_expr INC
      { $$ = expr_incdec(EX_POSTINC, $1, loc_pos(@2)); }
    | postfix_expr DEC
      { $$ = expr_incdec(EX_POSTDEC, $1, loc_pos(@2)); }
    ;

argument_expr_list_opt
    : /* пусто */          { $$ = NULL; }
    | argument_expr_list   { $$ = $1; }
    ;

argument_expr_list
    : assignment_expr
      {
          $$ = expr_comma(loc_pos(@1));
          expr_comma_add($$, $1);
      }
    | argument_expr_list ',' assignment_expr
      {
          expr_comma_add($1, $3);
          $$ = $1;
      }
    ;

primary_expr
    : IDENT
      { $$ = expr_ident($1, loc_pos(@1)); free($1); }
    | INT_LIT
      { $$ = expr_int_lit($1, loc_pos(@1)); }
    | FLOAT_LIT
      { $$ = expr_float_lit($1, loc_pos(@1)); }
    | CHAR_LIT
      { $$ = expr_char_lit($1, loc_pos(@1)); }
    | string_literal
      { $$ = $1; }
    | '(' expr ')'
      { $$ = $2; }
    ;

/* Соседние строковые литералы в C склеиваются. */
string_literal
    : STRING_LIT
      {
          $$ = expr_str_lit($1.data, $1.len, loc_pos(@1));
          free($1.data);
      }
    | string_literal STRING_LIT
      {
          int len = $1->u.str.len + $2.len;
          char *joined = (char *)xmalloc((size_t)len + 1);
          memcpy(joined, $1->u.str.data, (size_t)$1->u.str.len);
          memcpy(joined + $1->u.str.len, $2.data, (size_t)$2.len);
          joined[len] = '\0';
          free($1->u.str.data);
          $1->u.str.data = joined;
          $1->u.str.len = len;
          free($2.data);
          $$ = $1;
      }
    ;

%%

void yyerror(const char *msg)
{
    SrcPos pos;
    pos.line = yylloc.first_line;
    pos.col = yylloc.first_column;
    diag_error_at(pos, "%s", msg);
}

/* Общая часть обоих режимов запуска. */
static Program *finish_parse(void)
{
    int rc;

    cur_program = program_new();
    cur_base_type = NULL;
    cur_function = NULL;
    cur_declaration = NULL;

    rc = yyparse();
    lexer_finish();

    if (rc != 0 || diag_has_errors()) {
        /* Дерево заведомо неполное — отдавать его наружу нельзя. */
        program_free(cur_program);
        cur_program = NULL;
        return NULL;
    }
    return cur_program;
}

Program *parse_string(const char *src)
{
    comments_reset();
    lexer_start_string(src);
    return finish_parse();
}

Program *parse_file(FILE *fp)
{
    comments_reset();
    lexer_start_file(fp);
    return finish_parse();
}
