/* src/main.c — точка входа транслятора c2py.
 *
 * Порядок работы отражает классическую схему трансляции:
 *   лексический анализ -> синтаксический анализ -> семантический
 *   анализ -> генерация кода.
 * Каждый этап отделён от остальных и завершается до начала следующего.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codegen.h"
#include "comments.h"
#include "diag.h"
#include "parse.h"
#include "sema.h"
#include "symtab.h"
#include "types.h"
#include "util.h"

#define C2PY_VERSION "1.0"

static void print_usage(FILE *out, const char *program)
{
    fprintf(out,
            "c2py %s - translator from a subset of C to Python\n"
            "\n"
            "Usage:\n"
            "  %s [input.c] [-o output.py]\n"
            "\n"
            "Options:\n"
            "  -o FILE      write the result to FILE\n"
            "  -            read from standard input\n"
            "  --stdout     write the result to standard output\n"
            "  -h, --help   show this message\n"
            "  --version    show the version\n"
            "\n"
            "Without arguments reads input.c and writes output.py.\n",
            C2PY_VERSION, program);
}

/* Заменяет расширение имени файла на .py. */
static char *derive_output_name(const char *input)
{
    size_t len = strlen(input);
    const char *dot = strrchr(input, '.');
    char *out;

    if (dot != NULL && strchr(dot, '/') == NULL && strchr(dot, '\\') == NULL)
        len = (size_t)(dot - input);

    out = (char *)xmalloc(len + 4);
    memcpy(out, input, len);
    memcpy(out + len, ".py", 4);
    return out;
}

static char *read_all(FILE *fp)
{
    size_t cap = 4096, len = 0;
    char *buf = (char *)xmalloc(cap);
    size_t got;

    while ((got = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += got;
        if (len + 1 >= cap) {
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
    }
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    const char *input_name = NULL;
    char *output_name = NULL;
    int to_stdout = 0;
    int from_stdin = 0;
    int i;

    Program *prog = NULL;
    SymTab *tab = NULL;
    char *python = NULL;
    int status = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("c2py %s\n", C2PY_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--stdout") == 0) {
            to_stdout = 1;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "c2py: option -o requires a file name\n");
                return 2;
            }
            free(output_name);
            output_name = xstrdup(argv[++i]);
        } else if (strcmp(argv[i], "-") == 0) {
            from_stdin = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "c2py: unknown option '%s'\n", argv[i]);
            print_usage(stderr, argv[0]);
            return 2;
        } else {
            input_name = argv[i];
        }
    }

    /* Без аргументов работает так же, как прежняя версия транслятора:
     * input.c на входе, output.py на выходе. Если же имя файла задано,
     * имя результата получается из него заменой расширения. */
    if (input_name == NULL && !from_stdin) {
        input_name = "input.c";
        if (output_name == NULL && !to_stdout) output_name = xstrdup("output.py");
    }
    if (output_name == NULL && !to_stdout)
        output_name = derive_output_name(from_stdin ? "output" : input_name);

    diag_init(from_stdin ? "<stdin>" : input_name);

    /* --- лексический и синтаксический анализ --- */
    if (from_stdin) {
        char *text = read_all(stdin);
        prog = parse_string(text);
        free(text);
    } else {
        FILE *fp = fopen(input_name, "rb");
        if (fp == NULL) {
            SrcPos nowhere;
            nowhere.line = 0;
            nowhere.col = 0;
            diag_error_at(nowhere, "cannot open input file");
            status = 1;
            goto done;
        }
        prog = parse_file(fp);
        fclose(fp);
    }

    if (prog == NULL) {
        status = 1;
        goto done;
    }

    /* --- семантический анализ --- */
    tab = symtab_new();
    if (!sema_check(prog, tab)) {
        status = 1;
        goto done;
    }

    /* --- генерация кода --- */
    python = codegen_program(prog, tab);

    if (to_stdout) {
        fputs(python, stdout);
    } else {
        /* Двоичный режим: перевод строки остаётся одним байтом,
         * иначе результат зависел бы от операционной системы. */
        FILE *out = fopen(output_name, "wb");
        if (out == NULL) {
            fprintf(stderr, "c2py: cannot write to '%s'\n", output_name);
            status = 1;
            goto done;
        }
        fputs(python, out);
        fclose(out);
    }

done:
    if (status != 0)
        fprintf(stderr, "c2py: translation failed (%d error(s))\n",
                diag_error_count());

    free(python);
    program_free(prog);
    symtab_free(tab);
    free(output_name);
    comments_shutdown();
    types_shutdown();
    diag_shutdown();
    return status;
}
