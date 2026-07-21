/* src/types.h — система типов транслируемого подмножества языка C.
 *
 * Типы нужны не ради проверки корректности как таковой, а потому что
 * от них напрямую зависит генерируемый код Python:
 *   - int / int   -> целочисленное деление с усечением к нулю;
 *   - double / int -> обычное деление "/";
 *   - char        -> в C это целое, поэтому 'a' + 1 == 98.
 * Без типов эти случаи неразличимы и транслятор неизбежно ошибается.
 */
#ifndef C2PY_TYPES_H
#define C2PY_TYPES_H

#define TYPE_LENGTH_UNKNOWN (-1)

typedef enum {
    TY_ERROR = 0, /* результат уже сообщённой ошибки */
    TY_VOID,
    TY_CHAR,
    TY_SHORT,
    TY_INT,
    TY_LONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_ARRAY
} TypeKind;

typedef struct Type Type;

struct Type {
    TypeKind kind;
    const Type *elem; /* тип элемента для TY_ARRAY */
    int length;       /* длина массива либо TYPE_LENGTH_UNKNOWN */
};

/* Скалярные типы — разделяемые объекты, сравнимые по указателю. */
const Type *type_error(void);
const Type *type_void(void);
const Type *type_char(void);
const Type *type_short(void);
const Type *type_int(void);
const Type *type_long(void);
const Type *type_float(void);
const Type *type_double(void);

/* Массивы создаются по необходимости и кэшируются до конца работы. */
const Type *type_array(const Type *elem, int length);

int type_is_error(const Type *t);
int type_is_integer(const Type *t);
int type_is_floating(const Type *t);
int type_is_arithmetic(const Type *t);
int type_is_array(const Type *t);
int type_is_void(const Type *t);

const Type *type_element(const Type *t);
int         type_array_length(const Type *t);

/* Целочисленное повышение: char и short становятся int. */
const Type *type_promote(const Type *t);

/* Обычные арифметические преобразования для бинарной операции. */
const Type *type_usual_arith(const Type *a, const Type *b);

/* Допустимо ли присваивание значения типа src переменной типа dst. */
int type_assignable(const Type *dst, const Type *src);

/* Имя типа для диагностических сообщений (статический буфер). */
const char *type_name(const Type *t);

/* Освобождает кэш типов массивов. */
void types_shutdown(void);

#endif /* C2PY_TYPES_H */
