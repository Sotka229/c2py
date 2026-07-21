/* src/symtab.h — таблица символов с областями видимости.
 *
 * Кроме обычной задачи (связать использование имени с объявлением)
 * таблица решает специфическую для перевода в Python проблему имён.
 *
 * В C областью видимости является блок, а в Python — функция целиком.
 * Поэтому две разные переменные C с одинаковым именем в соседних или
 * вложенных блоках должны получить разные имена в Python. Кроме того,
 * имя переменной C может совпасть с ключевым словом Python.
 *
 * Каждому символу назначается py_name: имя, безопасное для Python
 * и уникальное в пределах функции.
 */
#ifndef C2PY_SYMTAB_H
#define C2PY_SYMTAB_H

#include "srcpos.h"
#include "types.h"

typedef enum { SYM_VAR, SYM_PARAM, SYM_FUNC } SymKind;

typedef struct Symbol {
    SymKind kind;
    char *name;    /* имя в исходном коде C   */
    char *py_name; /* имя в порождаемом коде  */
    const Type *type;
    int is_global;
    int depth;
    SrcPos pos;

    int used;     /* к символу обращались            */
    int assigned; /* символу присваивали значение    */

    /* Только для SYM_FUNC. */
    const Type **param_types;
    int nparams, param_cap;
    int has_body;
    int is_builtin;
    int is_variadic;

    /* Массив char, инициализированный строковым литералом:
     * в порождаемом коде представляется строкой Python. */
    int is_py_string;

    struct Symbol *scope_next; /* следующий символ той же области    */
    struct Symbol *all_next;   /* список владения всеми символами    */
} Symbol;

typedef struct SymTab SymTab;

SymTab *symtab_new(void);
void symtab_free(SymTab *t);

void symtab_push(SymTab *t);
void symtab_pop(SymTab *t);

/* Границы функции: внутри них имена Python обязаны быть уникальными. */
void symtab_begin_function(SymTab *t);
void symtab_end_function(SymTab *t);

/* Возвращает NULL, если имя уже объявлено в текущей области. */
Symbol *symtab_declare(SymTab *t, const char *name, SymKind kind,
                       const Type *type, SrcPos pos);

Symbol *symtab_lookup(SymTab *t, const char *name);
Symbol *symtab_lookup_local(SymTab *t, const char *name);

/* Запрещает использовать имя в порождаемом коде (служебные функции). */
void symtab_reserve(SymTab *t, const char *py_name);

void symtab_add_param_type(Symbol *fn, const Type *type);

/* Перечисление глобальных символов — нужно генератору кода. */
Symbol *symtab_globals(SymTab *t);

#endif /* C2PY_SYMTAB_H */
