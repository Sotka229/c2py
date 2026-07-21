/* src/types.c — реализация системы типов. */
#include "types.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Скалярные типы создаются один раз и живут всё время работы. */
static const Type ty_error = {TY_ERROR, NULL, 0};
static const Type ty_void = {TY_VOID, NULL, 0};
static const Type ty_char = {TY_CHAR, NULL, 0};
static const Type ty_short = {TY_SHORT, NULL, 0};
static const Type ty_int = {TY_INT, NULL, 0};
static const Type ty_long = {TY_LONG, NULL, 0};
static const Type ty_float = {TY_FLOAT, NULL, 0};
static const Type ty_double = {TY_DOUBLE, NULL, 0};

const Type *type_error(void) { return &ty_error; }
const Type *type_void(void) { return &ty_void; }
const Type *type_char(void) { return &ty_char; }
const Type *type_short(void) { return &ty_short; }
const Type *type_int(void) { return &ty_int; }
const Type *type_long(void) { return &ty_long; }
const Type *type_float(void) { return &ty_float; }
const Type *type_double(void) { return &ty_double; }

/* Кэш типов массивов: одинаковые типы представлены одним объектом,
 * поэтому их тоже можно сравнивать по указателю. */
typedef struct ArrayEntry {
    Type type;
    struct ArrayEntry *next;
} ArrayEntry;

static ArrayEntry *array_cache = NULL;

const Type *type_array(const Type *elem, int length)
{
    ArrayEntry *entry;

    if (elem == NULL || type_is_error(elem)) return type_error();

    for (entry = array_cache; entry != NULL; entry = entry->next) {
        if (entry->type.elem == elem && entry->type.length == length)
            return &entry->type;
    }

    entry = (ArrayEntry *)xmalloc(sizeof(ArrayEntry));
    entry->type.kind = TY_ARRAY;
    entry->type.elem = elem;
    entry->type.length = length;
    entry->next = array_cache;
    array_cache = entry;
    return &entry->type;
}

void types_shutdown(void)
{
    while (array_cache != NULL) {
        ArrayEntry *next = array_cache->next;
        free(array_cache);
        array_cache = next;
    }
}

int type_is_error(const Type *t) { return t != NULL && t->kind == TY_ERROR; }
int type_is_array(const Type *t) { return t != NULL && t->kind == TY_ARRAY; }
int type_is_void(const Type *t) { return t != NULL && t->kind == TY_VOID; }

int type_is_integer(const Type *t)
{
    if (t == NULL) return 0;
    return t->kind == TY_CHAR || t->kind == TY_SHORT || t->kind == TY_INT ||
           t->kind == TY_LONG;
}

int type_is_floating(const Type *t)
{
    if (t == NULL) return 0;
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE;
}

int type_is_arithmetic(const Type *t)
{
    return type_is_integer(t) || type_is_floating(t);
}

const Type *type_element(const Type *t)
{
    return type_is_array(t) ? t->elem : type_error();
}

int type_array_length(const Type *t)
{
    return type_is_array(t) ? t->length : TYPE_LENGTH_UNKNOWN;
}

const Type *type_promote(const Type *t)
{
    if (t == NULL) return type_error();
    if (t->kind == TY_CHAR || t->kind == TY_SHORT) return type_int();
    return t;
}

/* Ранг типа в обычных арифметических преобразованиях. */
static int arith_rank(const Type *t)
{
    switch (t->kind) {
    case TY_INT:    return 1;
    case TY_LONG:   return 2;
    case TY_FLOAT:  return 3;
    case TY_DOUBLE: return 4;
    default:        return 0;
    }
}

const Type *type_usual_arith(const Type *a, const Type *b)
{
    if (type_is_error(a) || type_is_error(b)) return type_error();
    if (!type_is_arithmetic(a) || !type_is_arithmetic(b)) return type_error();

    a = type_promote(a);
    b = type_promote(b);
    return arith_rank(a) >= arith_rank(b) ? a : b;
}

int type_assignable(const Type *dst, const Type *src)
{
    /* Ошибочный тип совместим со всем: сообщение уже выдано,
     * повторные жалобы только зашумят вывод. */
    if (type_is_error(dst) || type_is_error(src)) return 1;
    if (type_is_arithmetic(dst) && type_is_arithmetic(src)) return 1;
    return 0;
}

const char *type_name(const Type *t)
{
    /* Кольцо буферов позволяет вывести два типа в одном сообщении. */
    static char ring[4][64];
    static int slot = 0;
    char *buf;

    if (t == NULL) return "<null>";

    switch (t->kind) {
    case TY_ERROR:  return "<error>";
    case TY_VOID:   return "void";
    case TY_CHAR:   return "char";
    case TY_SHORT:  return "short";
    case TY_INT:    return "int";
    case TY_LONG:   return "long";
    case TY_FLOAT:  return "float";
    case TY_DOUBLE: return "double";
    case TY_ARRAY:
        buf = ring[slot];
        slot = (slot + 1) % 4;
        if (t->length == TYPE_LENGTH_UNKNOWN)
            snprintf(buf, 64, "%s[]", type_name(t->elem));
        else
            snprintf(buf, 64, "%s[%d]", type_name(t->elem), t->length);
        return buf;
    }
    return "<unknown>";
}
