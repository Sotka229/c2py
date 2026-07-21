/* src/lexer.l — лексический анализатор подмножества языка C.
 *
 * Отличия от исходного варианта:
 *   - лексер ничего не печатает, а только возвращает лексемы;
 *   - отслеживаются строка и столбец каждой лексемы;
 *   - поддержаны все формы литералов C и полный набор операций;
 *   - комментарии попадают в отдельную таблицу, а не в вывод;
 *   - незакрытый комментарий или строка дают внятное сообщение,
 *     а неизвестный символ не игнорируется молча.
 */
%{
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "comments.h"
#include "diag.h"
#include "escape.h"
#include "lexer.h"
#include "strbuf.h"
#include "util.h"
#include "parser.tab.h"

/* --- отслеживание позиции --- */

static int cur_line = 1;
static int cur_col = 1;
static int last_token_line = 0;

/* Ширина табуляции при подсчёте столбцов. */
#define TAB_WIDTH 8

static void update_location(void);

/* YY_USER_ACTION выполняется перед действием каждого правила,
 * поэтому позиция лексемы всегда актуальна. */
#define YY_USER_ACTION update_location();

static SrcPos here(void)
{
    SrcPos p;
    p.line = yylloc.first_line;
    p.col = yylloc.first_column;
    return p;
}

/* Отмечает, что на этой строке уже был код: следующий комментарий
 * на той же строке считается хвостовым. */
static void note_token(void) { last_token_line = yylloc.first_line; }

/* --- накопление текста комментария --- */

static StrBuf comment_text;
static int comment_ready = 0;
static int comment_line = 0;
static int comment_col = 0;

static void comment_begin(void)
{
    if (!comment_ready) {
        sb_init(&comment_text);
        comment_ready = 1;
    }
    sb_clear(&comment_text);
    comment_line = yylloc.first_line;
    comment_col = yylloc.first_column;
}

static void comment_end(void)
{
    comments_add(comment_line, cur_line, comment_col,
                 comment_line > last_token_line, sb_cstr(&comment_text));
}

/* --- разбор литералов --- */

static long long parse_integer(const char *text);
static void set_string_value(const char *text, int len);
static void set_char_value(const char *text, int len);

static YY_BUFFER_STATE active_buffer = NULL;
%}

%option yylineno
%option never-interactive

%x BLOCK_COMMENT
%x LINE_COMMENT

DIGIT     [0-9]
HEX       [0-9a-fA-F]
LETTER    [a-zA-Z_]
EXP       [eE][+-]?{DIGIT}+
ISUFFIX   [uUlL]+
FSUFFIX   [fFlL]

%%

    /* --- директивы препроцессора --- */

^[ \t]*#[ \t]*include[^\n]*    { /* заголовки C в Python не нужны */ }
^[ \t]*#[^\n]*                 {
                                 diag_warning_at(here(),
                                     "preprocessor directive ignored");
                               }

    /* --- комментарии --- */

"/*"                           { comment_begin(); BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"*/"            { comment_end(); BEGIN(INITIAL); }
<BLOCK_COMMENT>\n              { sb_putc(&comment_text, '\n'); }
<BLOCK_COMMENT>.               { sb_putc(&comment_text, yytext[0]); }
<BLOCK_COMMENT><<EOF>>         {
                                 SrcPos p;
                                 p.line = comment_line;
                                 p.col = comment_col;
                                 diag_error_at(p, "unterminated comment");
                                 comment_end();
                                 BEGIN(INITIAL);
                                 return 0;
                               }

"//"                           { comment_begin(); BEGIN(LINE_COMMENT); }
<LINE_COMMENT>[^\n]+           { sb_puts(&comment_text, yytext); }
<LINE_COMMENT>\n               { comment_end(); BEGIN(INITIAL); }
<LINE_COMMENT><<EOF>>          { comment_end(); BEGIN(INITIAL); return 0; }

    /* --- ключевые слова ---
       Перечислены до правила идентификатора: при одинаковой длине
       совпадения flex выбирает правило, записанное раньше. */

"int"          { note_token(); return KW_INT; }
"char"         { note_token(); return KW_CHAR; }
"float"        { note_token(); return KW_FLOAT; }
"double"       { note_token(); return KW_DOUBLE; }
"void"         { note_token(); return KW_VOID; }
"long"         { note_token(); return KW_LONG; }
"short"        { note_token(); return KW_SHORT; }
"unsigned"     { note_token(); return KW_UNSIGNED; }
"signed"       { note_token(); return KW_SIGNED; }
"const"        { note_token(); return KW_CONST; }
"sizeof"       { note_token(); return KW_SIZEOF; }
"if"           { note_token(); return KW_IF; }
"else"         { note_token(); return KW_ELSE; }
"while"        { note_token(); return KW_WHILE; }
"do"           { note_token(); return KW_DO; }
"for"          { note_token(); return KW_FOR; }
"switch"       { note_token(); return KW_SWITCH; }
"case"         { note_token(); return KW_CASE; }
"default"      { note_token(); return KW_DEFAULT; }
"break"        { note_token(); return KW_BREAK; }
"continue"     { note_token(); return KW_CONTINUE; }
"return"       { note_token(); return KW_RETURN; }

    /* --- идентификаторы --- */

{LETTER}({LETTER}|{DIGIT})*    {
                                 note_token();
                                 yylval.sval = xstrdup(yytext);
                                 return IDENT;
                               }

    /* --- вещественные литералы ---
       Записаны до целых: "1.5" должно распознаваться целиком. */

{DIGIT}+"."{DIGIT}*{EXP}?{FSUFFIX}?   |
"."{DIGIT}+{EXP}?{FSUFFIX}?           |
{DIGIT}+{EXP}{FSUFFIX}?               {
                                 note_token();
                                 yylval.fval = strtod(yytext, NULL);
                                 return FLOAT_LIT;
                               }

    /* --- целые литералы --- */

0[xX]{HEX}+{ISUFFIX}?          |
0[0-7]+{ISUFFIX}?              |
{DIGIT}+{ISUFFIX}?             {
                                 note_token();
                                 yylval.ival = parse_integer(yytext);
                                 return INT_LIT;
                               }

    /* --- символьные и строковые литералы --- */

'([^'\\\n]|\\(.|\n))*'         {
                                 note_token();
                                 set_char_value(yytext + 1, yyleng - 2);
                                 return CHAR_LIT;
                               }
'([^'\\\n]|\\(.|\n))*          {
                                 note_token();
                                 diag_error_at(here(),
                                     "unterminated character literal");
                                 yylval.ival = 0;
                                 return CHAR_LIT;
                               }

\"([^"\\\n]|\\(.|\n))*\"       {
                                 note_token();
                                 set_string_value(yytext + 1, yyleng - 2);
                                 return STRING_LIT;
                               }
\"([^"\\\n]|\\(.|\n))*         {
                                 note_token();
                                 diag_error_at(here(),
                                     "unterminated string literal");
                                 set_string_value(yytext + 1, yyleng - 1);
                                 return STRING_LIT;
                               }

    /* --- составные операции ---
       Длина совпадения решает: ">>=" не распадётся на ">>" и "=". */

"<<="   { note_token(); return SHL_ASSIGN; }
">>="   { note_token(); return SHR_ASSIGN; }
"+="    { note_token(); return ADD_ASSIGN; }
"-="    { note_token(); return SUB_ASSIGN; }
"*="    { note_token(); return MUL_ASSIGN; }
"/="    { note_token(); return DIV_ASSIGN; }
"%="    { note_token(); return MOD_ASSIGN; }
"&="    { note_token(); return AND_ASSIGN; }
"|="    { note_token(); return OR_ASSIGN; }
"^="    { note_token(); return XOR_ASSIGN; }
"<<"    { note_token(); return SHL; }
">>"    { note_token(); return SHR; }
"++"    { note_token(); return INC; }
"--"    { note_token(); return DEC; }
"<="    { note_token(); return LE; }
">="    { note_token(); return GE; }
"=="    { note_token(); return EQ; }
"!="    { note_token(); return NE; }
"&&"    { note_token(); return AND_OP; }
"||"    { note_token(); return OR_OP; }

    /* --- одиночные символы --- */

[-+*/%=<>!~&|^(){}\[\];,?:.]   { note_token(); return yytext[0]; }

    /* --- пробельные символы --- */

[ \t\r\n]+                     { /* пропускаются */ }

    /* --- всё остальное --- */

.                              {
                                 diag_error_at(here(),
                                     "stray character '%c' in program",
                                     yytext[0]);
                               }

%%

/* Обновляет текущую позицию и записывает её в yylloc. */
static void update_location(void)
{
    int i;

    yylloc.first_line = cur_line;
    yylloc.first_column = cur_col;

    for (i = 0; i < yyleng; i++) {
        if (yytext[i] == '\n') {
            cur_line++;
            cur_col = 1;
        } else if (yytext[i] == '\t') {
            cur_col = ((cur_col - 1) / TAB_WIDTH + 1) * TAB_WIDTH + 1;
        } else {
            cur_col++;
        }
    }

    yylloc.last_line = cur_line;
    yylloc.last_column = cur_col;
}

/* Разбирает целый литерал во всех трёх системах счисления
 * и отбрасывает суффиксы U/L. */
static long long parse_integer(const char *text)
{
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        return (long long)strtoull(text + 2, NULL, 16);
    if (text[0] == '0' && text[1] != '\0')
        return (long long)strtoull(text + 1, NULL, 8);
    return (long long)strtoull(text, NULL, 10);
}

static void set_string_value(const char *text, int len)
{
    int out_len = 0;
    if (len < 0) len = 0;
    yylval.str.data = escape_decode(text, len, &out_len, here());
    yylval.str.len = out_len;
}

static void set_char_value(const char *text, int len)
{
    int out_len = 0;
    char *decoded;

    if (len < 0) len = 0;
    decoded = escape_decode(text, len, &out_len, here());

    if (out_len == 0) {
        diag_error_at(here(), "empty character literal");
        yylval.ival = 0;
    } else {
        if (out_len > 1)
            diag_warning_at(here(),
                            "multi-character literal, only first byte used");
        /* В C символьный литерал имеет тип int. */
        yylval.ival = (long long)(unsigned char)decoded[0];
    }
    free(decoded);
}

/* --- управление буфером --- */

static void lexer_reset_state(void)
{
    cur_line = 1;
    cur_col = 1;
    last_token_line = 0;
    yylineno = 1;
    BEGIN(INITIAL); /* не наследуем состояние комментария от прошлого разбора */
}

void lexer_start_string(const char *src)
{
    lexer_finish();
    lexer_reset_state();
    active_buffer = yy_scan_string(src);
}

void lexer_start_file(FILE *fp)
{
    lexer_finish();
    lexer_reset_state();
    active_buffer = yy_create_buffer(fp, YY_BUF_SIZE);
    yy_switch_to_buffer(active_buffer);
}

void lexer_finish(void)
{
    if (active_buffer != NULL) {
        yy_delete_buffer(active_buffer);
        active_buffer = NULL;
    }
    if (comment_ready) {
        sb_free(&comment_text);
        comment_ready = 0;
    }
}

int yywrap(void) { return 1; }
