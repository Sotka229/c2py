/* src/util.c — реализация вспомогательных функций. */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void out_of_memory(void)
{
    fprintf(stderr, "c2py: out of memory\n");
    exit(2);
}

void *xmalloc(size_t size)
{
    void *p;
    if (size == 0) size = 1;
    p = malloc(size);
    if (p == NULL) out_of_memory();
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p;
    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }
    p = calloc(count, size);
    if (p == NULL) out_of_memory();
    return p;
}

void *xrealloc(void *ptr, size_t size)
{
    void *p;
    if (size == 0) size = 1;
    p = realloc(ptr, size);
    if (p == NULL) out_of_memory();
    return p;
}

char *xstrdup(const char *s)
{
    size_t n;
    char *copy;
    if (s == NULL) return NULL;
    n = strlen(s);
    copy = (char *)xmalloc(n + 1);
    memcpy(copy, s, n + 1);
    return copy;
}

char *xstrndup(const char *s, size_t n)
{
    size_t len = 0;
    char *copy;
    if (s == NULL) return NULL;
    while (len < n && s[len] != '\0') len++;
    copy = (char *)xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
