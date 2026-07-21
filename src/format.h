/* src/format.h — разбор строки формата printf/scanf.
 *
 * Оператор форматирования "%" в Python унаследован от C, поэтому
 * почти все спецификаторы совпадают: %d, %5.2f, %-10s, %x, %o, %e, %g
 * работают одинаково. Расхождений всего три:
 *   - модификаторы длины (l, ll, h, z) в Python не нужны;
 *   - %u и %i в Python отсутствуют — оба заменяются на %d;
 *   - %p не поддерживается вовсе.
 *
 * Поэтому строка формата не переписывается заново, а нормализуется:
 * так сохраняются ширина поля, точность и флаги, которые исходный
 * вариант транслятора терял.
 */
#ifndef C2PY_FORMAT_H
#define C2PY_FORMAT_H

typedef enum {
    CONV_INT,     /* d, i          */
    CONV_UINT,    /* u             */
    CONV_HEX,     /* x, X          */
    CONV_OCT,     /* o             */
    CONV_FLOAT,   /* f, F          */
    CONV_EXP,     /* e, E          */
    CONV_GENERAL, /* g, G          */
    CONV_CHAR,    /* c             */
    CONV_STRING,  /* s             */
    CONV_UNKNOWN  /* неподдерживаемая конверсия */
} ConvKind;

typedef struct FmtString FmtString;

/* Разбирает строку формата (байты уже раскодированы лексером). */
FmtString *fmt_parse(const char *data, int len);
void fmt_free(FmtString *f);

/* Число конверсий, требующих аргумент ("%%" не считается). */
int fmt_conversion_count(const FmtString *f);

/* Полное число аргументов с учётом "*" в ширине и точности. */
int fmt_arg_count(const FmtString *f);

ConvKind fmt_conversion_kind(const FmtString *f, int index);

/* Позиция аргумента для index-й конверсии (с учётом "*"). */
int fmt_conversion_arg_index(const FmtString *f, int index);

int fmt_has_unsupported(const FmtString *f);
const char *fmt_unsupported_spec(const FmtString *f);

/* Строка формата, пригодная для оператора "%" в Python. */
const char *fmt_python(const FmtString *f, int *out_len);

/* Тот же текст, но с заменой "%%" на "%": используется, когда
 * конверсий нет и форматирование не нужно. */
const char *fmt_plain(const FmtString *f, int *out_len);

#endif /* C2PY_FORMAT_H */
