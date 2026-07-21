/* src/symtab.c — реализация таблицы символов. */
#include "symtab.h"
#include "strbuf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ключевые слова Python и имена, которые нельзя переопределять:
 * переменная C с таким именем получит изменённое имя. */
static const char *const PY_RESERVED[] = {
    "False",  "None",   "True",   "and",      "as",       "assert", "async",
    "await",  "break",  "class",  "continue", "def",      "del",    "elif",
    "else",   "except", "finally", "for",     "from",     "global", "if",
    "import", "in",     "is",     "lambda",   "nonlocal", "not",    "or",
    "pass",   "raise",  "return", "try",      "while",    "with",   "yield",
    /* встроенные имена, которые использует порождаемый код */
    "print",  "input",  "range",  "len",      "int",      "float",  "str",
    "chr",    "ord",    "abs",    "exit",     "sys",      "math",
    NULL};

/* Область видимости. */
typedef struct Scope {
    Symbol *symbols; /* список символов области */
    struct Scope *parent;
    int depth;
} Scope;

/* Занятое имя Python в пределах текущей функции. */
typedef struct TakenName {
    char *name;
    struct TakenName *next;
} TakenName;

struct SymTab {
    Scope *current;
    Scope *global;
    TakenName *taken;    /* имена, занятые в текущей функции */
    TakenName *reserved; /* имена, занятые всегда            */
    Symbol *all;         /* полный список для освобождения   */
    Symbol *all_tail;
};

static int is_py_reserved(const char *name)
{
    int i;
    for (i = 0; PY_RESERVED[i] != NULL; i++)
        if (strcmp(PY_RESERVED[i], name) == 0) return 1;
    return 0;
}

static int name_is_taken(SymTab *t, const char *name)
{
    TakenName *n;
    for (n = t->taken; n != NULL; n = n->next)
        if (strcmp(n->name, name) == 0) return 1;
    for (n = t->reserved; n != NULL; n = n->next)
        if (strcmp(n->name, name) == 0) return 1;
    return 0;
}

static void take_name(SymTab *t, const char *name)
{
    TakenName *n = (TakenName *)xmalloc(sizeof(TakenName));
    n->name = xstrdup(name);
    n->next = t->taken;
    t->taken = n;
}

static void free_names(TakenName *n)
{
    while (n != NULL) {
        TakenName *next = n->next;
        free(n->name);
        free(n);
        n = next;
    }
}

/* Подбирает имя, свободное в текущей функции и допустимое в Python. */
static char *make_py_name(SymTab *t, const char *name)
{
    StrBuf sb;
    int suffix;

    if (!is_py_reserved(name) && !name_is_taken(t, name)) {
        take_name(t, name);
        return xstrdup(name);
    }

    /* Занятое или запрещённое имя дополняется номером. */
    sb_init(&sb);
    for (suffix = 1;; suffix++) {
        sb_clear(&sb);
        sb_printf(&sb, "%s_%d", name, suffix);
        if (!is_py_reserved(sb_cstr(&sb)) && !name_is_taken(t, sb_cstr(&sb)))
            break;
    }
    take_name(t, sb_cstr(&sb));
    return sb_detach(&sb);
}

static Scope *scope_new(Scope *parent, int depth)
{
    Scope *s = (Scope *)xcalloc(1, sizeof(Scope));
    s->parent = parent;
    s->depth = depth;
    return s;
}

SymTab *symtab_new(void)
{
    SymTab *t = (SymTab *)xcalloc(1, sizeof(SymTab));
    t->global = scope_new(NULL, 0);
    t->current = t->global;
    return t;
}

void symtab_free(SymTab *t)
{
    Symbol *s;
    if (t == NULL) return;

    /* Области, кроме глобальной, к этому моменту уже сняты. */
    while (t->current != NULL && t->current != t->global) symtab_pop(t);

    s = t->all;
    while (s != NULL) {
        Symbol *next = s->all_next;
        free(s->name);
        free(s->py_name);
        free(s->param_types);
        free(s);
        s = next;
    }
    free_names(t->taken);
    free_names(t->reserved);
    free(t->global);
    free(t);
}

void symtab_push(SymTab *t)
{
    t->current = scope_new(t->current, t->current->depth + 1);
}

void symtab_pop(SymTab *t)
{
    Scope *dead = t->current;
    if (dead == t->global) return;
    t->current = dead->parent;
    free(dead); /* сами символы освобождаются в symtab_free */
}

void symtab_begin_function(SymTab *t)
{
    free_names(t->taken);
    t->taken = NULL;
}

void symtab_end_function(SymTab *t)
{
    free_names(t->taken);
    t->taken = NULL;
}

void symtab_reserve(SymTab *t, const char *py_name)
{
    TakenName *n = (TakenName *)xmalloc(sizeof(TakenName));
    n->name = xstrdup(py_name);
    n->next = t->reserved;
    t->reserved = n;
}

Symbol *symtab_declare(SymTab *t, const char *name, SymKind kind,
                       const Type *type, SrcPos pos)
{
    Symbol *s;

    if (symtab_lookup_local(t, name) != NULL) return NULL;

    s = (Symbol *)xcalloc(1, sizeof(Symbol));
    s->kind = kind;
    s->name = xstrdup(name);
    s->type = type;
    s->pos = pos;
    s->depth = t->current->depth;
    s->is_global = (t->current == t->global);

    /* Глобальные имена и имена функций уникальны по правилам C,
     * но всё ещё могут совпасть с ключевым словом Python. */
    s->py_name = make_py_name(t, name);

    /* Символ попадает в цепочку своей области... */
    s->scope_next = t->current->symbols;
    t->current->symbols = s;

    /* ...и в общий список, который владеет памятью. Область можно
     * снять, не освобождая символы: на них ещё ссылается дерево. */
    s->all_next = NULL;
    if (t->all_tail == NULL)
        t->all = t->all_tail = s;
    else {
        t->all_tail->all_next = s;
        t->all_tail = s;
    }
    return s;
}

/* Поиск идёт от текущей области к глобальной, поэтому объявление
 * во вложенном блоке закрывает одноимённое внешнее. */
Symbol *symtab_lookup(SymTab *t, const char *name)
{
    Scope *scope;
    for (scope = t->current; scope != NULL; scope = scope->parent) {
        Symbol *s;
        for (s = scope->symbols; s != NULL; s = s->scope_next)
            if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

Symbol *symtab_lookup_local(SymTab *t, const char *name)
{
    Symbol *s;
    for (s = t->current->symbols; s != NULL; s = s->scope_next)
        if (strcmp(s->name, name) == 0) return s;
    return NULL;
}

void symtab_add_param_type(Symbol *fn, const Type *type)
{
    if (fn->nparams == fn->param_cap) {
        fn->param_cap = fn->param_cap ? fn->param_cap * 2 : 4;
        fn->param_types = (const Type **)xrealloc(
            fn->param_types, (size_t)fn->param_cap * sizeof(const Type *));
    }
    fn->param_types[fn->nparams++] = type;
}

Symbol *symtab_globals(SymTab *t) { return t->global->symbols; }
