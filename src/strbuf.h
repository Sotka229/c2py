/* src/strbuf.h — динамический строковый буфер.
 *
 * Используется генератором кода: результат трансляции собирается
 * в памяти и лишь затем выводится, что позволяет откладывать
 * решения (например, дописывать объявления global в начало функции).
 *
 * Буфер корректно хранит нулевые байты внутри данных и всегда
 * поддерживает завершающий ноль, чтобы sb_cstr() был безопасен.
 */
#ifndef C2PY_STRBUF_H
#define C2PY_STRBUF_H

#include <stddef.h>

typedef struct {
    char  *data; /* NULL, пока не было ни одной записи */
    size_t len;  /* длина без завершающего нуля        */
    size_t cap;  /* размер выделенной памяти           */
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_clear(StrBuf *sb);

void sb_putc(StrBuf *sb, char c);
void sb_write(StrBuf *sb, const char *bytes, size_t n);
void sb_puts(StrBuf *sb, const char *s);

/* Снимает n последних байтов (не больше длины буфера). */
void sb_chop(StrBuf *sb, size_t n);

#if defined(__GNUC__)
void sb_printf(StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void sb_printf(StrBuf *sb, const char *fmt, ...);
#endif

const char *sb_cstr(const StrBuf *sb);
size_t      sb_len(const StrBuf *sb);

/* Передаёт владение строкой вызывающему (её нужно free);
 * сам буфер после вызова снова пуст и пригоден к работе. */
char *sb_detach(StrBuf *sb);

#endif /* C2PY_STRBUF_H */
