/* src/strbuf.c — реализация динамического строкового буфера. */
#include "strbuf.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SB_MIN_CAP 64

/* Гарантирует место ещё под extra байт плюс завершающий ноль. */
static void sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;

    if (sb->data != NULL && need <= sb->cap) return;

    cap = sb->cap ? sb->cap : SB_MIN_CAP;
    while (cap < need) cap *= 2;

    sb->data = (char *)xrealloc(sb->data, cap);
    sb->cap = cap;
}

void sb_init(StrBuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_clear(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data != NULL) sb->data[0] = '\0';
}

void sb_putc(StrBuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_write(StrBuf *sb, const char *bytes, size_t n)
{
    if (n == 0) {
        sb_reserve(sb, 0);
        sb->data[sb->len] = '\0';
        return;
    }
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, bytes, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s)
{
    if (s == NULL) return;
    sb_write(sb, s, strlen(s));
}

/* Снимает n последних байтов: генератору кода это нужно, чтобы
 * убрать уже выведенный перевод строки и дописать в конец строки
 * хвостовой комментарий. */
void sb_chop(StrBuf *sb, size_t n)
{
    if (sb->data == NULL) return;
    sb->len = (n >= sb->len) ? 0 : sb->len - n;
    sb->data[sb->len] = '\0';
}

/* Форматирует прямо в буфер, при нехватке места увеличивая его.
 *
 * Реализации vsnprintf расходятся в поведении при нехватке места:
 * C99 возвращает требуемую длину, а старая библиотека MSVCRT
 * (её использует MinGW при -std=c99) возвращает отрицательное число.
 * Цикл ниже корректно работает в обоих случаях. */
void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        int written;
        size_t space;

        sb_reserve(sb, 0); /* гарантируем, что data выделен */
        space = sb->cap - sb->len;

        va_start(ap, fmt);
        written = vsnprintf(sb->data + sb->len, space, fmt, ap);
        va_end(ap);

        if (written >= 0 && (size_t)written < space) {
            sb->len += (size_t)written;
            sb->data[sb->len] = '\0';
            return;
        }

        if (written >= 0)
            sb_reserve(sb, (size_t)written); /* известна точная длина */
        else
            sb_reserve(sb, space * 2); /* длина неизвестна — удваиваем */
    }
}

const char *sb_cstr(const StrBuf *sb)
{
    return sb->data != NULL ? sb->data : "";
}

size_t sb_len(const StrBuf *sb)
{
    return sb->len;
}

char *sb_detach(StrBuf *sb)
{
    char *owned;
    if (sb->data == NULL) {
        sb_reserve(sb, 0);
        sb->data[0] = '\0';
    }
    owned = sb->data;
    sb_init(sb);
    return owned;
}
