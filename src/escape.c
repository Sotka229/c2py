/* src/escape.c — раскодирование и кодирование escape-последовательностей. */
#include "escape.h"
#include "diag.h"
#include "strbuf.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *escape_decode(const char *src, int len, int *out_len, SrcPos pos)
{
    char *out = (char *)xmalloc((size_t)len + 1);
    int i = 0, n = 0;

    while (i < len) {
        if (src[i] != '\\') {
            out[n++] = src[i++];
            continue;
        }

        i++; /* пропускаем обратную косую черту */
        if (i >= len) {
            diag_error_at(pos, "stray backslash at end of literal");
            break;
        }

        switch (src[i]) {
        case 'n':  out[n++] = '\n'; i++; break;
        case 't':  out[n++] = '\t'; i++; break;
        case 'r':  out[n++] = '\r'; i++; break;
        case 'a':  out[n++] = '\a'; i++; break;
        case 'b':  out[n++] = '\b'; i++; break;
        case 'f':  out[n++] = '\f'; i++; break;
        case 'v':  out[n++] = '\v'; i++; break;
        case '\\': out[n++] = '\\'; i++; break;
        case '\'': out[n++] = '\''; i++; break;
        case '"':  out[n++] = '"';  i++; break;
        case '?':  out[n++] = '?';  i++; break;
        case '\n': i++; break; /* перенос строки внутри литерала */

        case 'x': {
            int value = 0, digits = 0;
            i++;
            while (i < len && hex_value((unsigned char)src[i]) >= 0) {
                value = value * 16 + hex_value((unsigned char)src[i]);
                i++;
                digits++;
            }
            if (digits == 0)
                diag_error_at(pos, "\\x used with no hexadecimal digits");
            out[n++] = (char)(value & 0xFF);
            break;
        }

        default:
            if (src[i] >= '0' && src[i] <= '7') {
                int value = 0, digits = 0;
                while (i < len && digits < 3 && src[i] >= '0' && src[i] <= '7') {
                    value = value * 8 + (src[i] - '0');
                    i++;
                    digits++;
                }
                out[n++] = (char)(value & 0xFF);
            } else {
                diag_warning_at(pos, "unknown escape sequence '\\%c'", src[i]);
                out[n++] = src[i];
                i++;
            }
            break;
        }
    }

    out[n] = '\0';
    if (out_len != NULL) *out_len = n;
    return out;
}

char *escape_to_python_string(const char *data, int len)
{
    StrBuf sb;
    int i;

    sb_init(&sb);
    sb_putc(&sb, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (c) {
        case '\n': sb_puts(&sb, "\\n"); break;
        case '\t': sb_puts(&sb, "\\t"); break;
        case '\r': sb_puts(&sb, "\\r"); break;
        case '\\': sb_puts(&sb, "\\\\"); break;
        case '"':  sb_puts(&sb, "\\\""); break;
        case '\a': sb_puts(&sb, "\\a"); break;
        case '\b': sb_puts(&sb, "\\b"); break;
        case '\f': sb_puts(&sb, "\\f"); break;
        case '\v': sb_puts(&sb, "\\v"); break;
        case '\0': sb_puts(&sb, "\\x00"); break;
        default:
            /* Управляющие символы выводим в шестнадцатеричном виде,
             * прочие байты (включая UTF-8) — как есть. */
            if (c < 0x20 || c == 0x7F)
                sb_printf(&sb, "\\x%02x", c);
            else
                sb_putc(&sb, (char)c);
            break;
        }
    }
    sb_putc(&sb, '"');
    return sb_detach(&sb);
}
