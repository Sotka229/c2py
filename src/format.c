/* src/format.c — реализация разбора строки формата. */
#include "format.h"
#include "strbuf.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ConvKind kind;
    int arg_index; /* номер аргумента, которому соответствует конверсия */
} Conversion;

struct FmtString {
    Conversion *convs;
    int count, cap;
    int arg_count;    /* с учётом "*" в ширине и точности */
    int unsupported;  /* найдена неподдерживаемая конверсия */
    char bad_spec[16];
    StrBuf python; /* нормализованная строка формата */
    StrBuf plain;  /* она же, но с "%%" -> "%"        */
};

static void add_conversion(FmtString *f, ConvKind kind, int arg_index)
{
    if (f->count == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->convs =
            (Conversion *)xrealloc(f->convs, (size_t)f->cap * sizeof(Conversion));
    }
    f->convs[f->count].kind = kind;
    f->convs[f->count].arg_index = arg_index;
    f->count++;
}

static void append_both(FmtString *f, const char *s, int n)
{
    sb_write(&f->python, s, (size_t)n);
    sb_write(&f->plain, s, (size_t)n);
}

static int is_flag(char c)
{
    return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '\'';
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

FmtString *fmt_parse(const char *data, int len)
{
    FmtString *f = (FmtString *)xcalloc(1, sizeof(FmtString));
    int i = 0;

    sb_init(&f->python);
    sb_init(&f->plain);

    while (i < len) {
        int start, has_star_width = 0, has_star_prec = 0;
        char conv;
        StrBuf spec;

        if (data[i] != '%') {
            append_both(f, data + i, 1);
            i++;
            continue;
        }

        /* "%%" — литеральный процент. */
        if (i + 1 < len && data[i + 1] == '%') {
            sb_puts(&f->python, "%%"); /* для оператора "%" нужен двойной */
            sb_putc(&f->plain, '%');   /* при печати напрямую — одинарный */
            i += 2;
            continue;
        }

        start = i;
        i++; /* пропускаем '%' */

        sb_init(&spec);
        sb_putc(&spec, '%');

        while (i < len && is_flag(data[i])) sb_putc(&spec, data[i++]);

        if (i < len && data[i] == '*') {
            has_star_width = 1;
            sb_putc(&spec, data[i++]);
        } else {
            while (i < len && is_digit(data[i])) sb_putc(&spec, data[i++]);
        }

        if (i < len && data[i] == '.') {
            sb_putc(&spec, data[i++]);
            if (i < len && data[i] == '*') {
                has_star_prec = 1;
                sb_putc(&spec, data[i++]);
            } else {
                while (i < len && is_digit(data[i])) sb_putc(&spec, data[i++]);
            }
        }

        /* Модификаторы длины в Python не нужны — просто пропускаем. */
        while (i < len && (data[i] == 'h' || data[i] == 'l' || data[i] == 'L' ||
                           data[i] == 'z' || data[i] == 'j' || data[i] == 't'))
            i++;

        if (i >= len) {
            /* Строка оборвалась на спецификаторе. */
            f->unsupported = 1;
            snprintf(f->bad_spec, sizeof(f->bad_spec), "%.*s", len - start,
                     data + start);
            append_both(f, data + start, len - start);
            sb_free(&spec);
            break;
        }

        conv = data[i++];

        if (has_star_width) f->arg_count++;
        if (has_star_prec) f->arg_count++;

        switch (conv) {
        case 'd':
        case 'i':
            add_conversion(f, CONV_INT, f->arg_count++);
            sb_putc(&spec, 'd');
            break;
        case 'u':
            /* В Python нет %u: печатаем как знаковое. */
            add_conversion(f, CONV_UINT, f->arg_count++);
            sb_putc(&spec, 'd');
            break;
        case 'x':
        case 'X':
            add_conversion(f, CONV_HEX, f->arg_count++);
            sb_putc(&spec, conv);
            break;
        case 'o':
            add_conversion(f, CONV_OCT, f->arg_count++);
            sb_putc(&spec, 'o');
            break;
        case 'f':
        case 'F':
            add_conversion(f, CONV_FLOAT, f->arg_count++);
            sb_putc(&spec, 'f');
            break;
        case 'e':
        case 'E':
            add_conversion(f, CONV_EXP, f->arg_count++);
            sb_putc(&spec, conv);
            break;
        case 'g':
        case 'G':
            add_conversion(f, CONV_GENERAL, f->arg_count++);
            sb_putc(&spec, conv);
            break;
        case 'c':
            add_conversion(f, CONV_CHAR, f->arg_count++);
            sb_putc(&spec, 'c');
            break;
        case 's':
            add_conversion(f, CONV_STRING, f->arg_count++);
            sb_putc(&spec, 's');
            break;
        default:
            f->unsupported = 1;
            snprintf(f->bad_spec, sizeof(f->bad_spec), "%%%c", conv);
            add_conversion(f, CONV_UNKNOWN, f->arg_count++);
            sb_putc(&spec, conv);
            break;
        }

        append_both(f, sb_cstr(&spec), (int)sb_len(&spec));
        sb_free(&spec);
    }

    return f;
}

void fmt_free(FmtString *f)
{
    if (f == NULL) return;
    free(f->convs);
    sb_free(&f->python);
    sb_free(&f->plain);
    free(f);
}

int fmt_conversion_count(const FmtString *f) { return f->count; }
int fmt_arg_count(const FmtString *f) { return f->arg_count; }

ConvKind fmt_conversion_kind(const FmtString *f, int index)
{
    if (index < 0 || index >= f->count) return CONV_UNKNOWN;
    return f->convs[index].kind;
}

int fmt_conversion_arg_index(const FmtString *f, int index)
{
    if (index < 0 || index >= f->count) return -1;
    return f->convs[index].arg_index;
}

int fmt_has_unsupported(const FmtString *f) { return f->unsupported; }

const char *fmt_unsupported_spec(const FmtString *f) { return f->bad_spec; }

const char *fmt_python(const FmtString *f, int *out_len)
{
    if (out_len != NULL) *out_len = (int)sb_len(&f->python);
    return sb_cstr(&f->python);
}

const char *fmt_plain(const FmtString *f, int *out_len)
{
    if (out_len != NULL) *out_len = (int)sb_len(&f->plain);
    return sb_cstr(&f->plain);
}
