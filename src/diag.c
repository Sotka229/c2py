/* src/diag.c — реализация подсистемы диагностики. */
#include "diag.h"
#include "strbuf.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *diag_file = NULL;
static int diag_errors = 0;
static int diag_warnings = 0;
static int diag_printed = 0;
static int diag_capped = 0;

void diag_init(const char *filename)
{
    free(diag_file);
    diag_file = xstrdup(filename != NULL ? filename : "<input>");
    diag_errors = 0;
    diag_warnings = 0;
    diag_printed = 0;
    diag_capped = 0;
}

void diag_shutdown(void)
{
    free(diag_file);
    diag_file = NULL;
}

/* Общая часть вывода сообщения любого уровня. */
static void emit(const char *level, SrcPos pos, const char *fmt, va_list ap)
{
    StrBuf line;

    if (diag_printed >= DIAG_MAX_REPORTED) {
        if (!diag_capped) {
            diag_capped = 1;
            fprintf(stderr,
                    "c2py: too many errors, further messages suppressed\n");
        }
        return;
    }
    diag_printed++;

    sb_init(&line);
    /* Строка 0 означает, что точное место неизвестно. */
    if (pos.line > 0)
        sb_printf(&line, "%s:%d:%d: %s: ", diag_file ? diag_file : "<input>",
                  pos.line, pos.col > 0 ? pos.col : 1, level);
    else
        sb_printf(&line, "%s: %s: ", diag_file ? diag_file : "<input>", level);

    {
        /* Текст сообщения форматируется отдельно: его длина не ограничена. */
        va_list copy;
        int needed;

        va_copy(copy, ap);
        needed = vsnprintf(NULL, 0, fmt, copy);
        va_end(copy);
        if (needed < 0) needed = 0;

        {
            char *tmp = (char *)xmalloc((size_t)needed + 1);
            vsnprintf(tmp, (size_t)needed + 1, fmt, ap);
            sb_puts(&line, tmp);
            free(tmp);
        }
    }
    sb_putc(&line, '\n');

    fputs(sb_cstr(&line), stderr);
    sb_free(&line);
}

void diag_error_at(SrcPos pos, const char *fmt, ...)
{
    va_list ap;
    diag_errors++;
    va_start(ap, fmt);
    emit("error", pos, fmt, ap);
    va_end(ap);
}

void diag_warning_at(SrcPos pos, const char *fmt, ...)
{
    va_list ap;
    diag_warnings++;
    va_start(ap, fmt);
    emit("warning", pos, fmt, ap);
    va_end(ap);
}

int diag_error_count(void) { return diag_errors; }
int diag_warning_count(void) { return diag_warnings; }
int diag_has_errors(void) { return diag_errors > 0; }
