/* src/comments.c — реализация таблицы комментариев. */
#include "comments.h"
#include "util.h"

#include <stdlib.h>

static Comment *items = NULL;
static int count = 0;
static int cap = 0;

void comments_reset(void)
{
    int i;
    for (i = 0; i < count; i++) free(items[i].text);
    count = 0;
}

void comments_add(int line, int end_line, int col, int own_line,
                  const char *text)
{
    if (count == cap) {
        cap = cap ? cap * 2 : 16;
        items = (Comment *)xrealloc(items, (size_t)cap * sizeof(Comment));
    }
    items[count].line = line;
    items[count].end_line = end_line;
    items[count].col = col;
    items[count].own_line = own_line;
    items[count].text = xstrdup(text);
    count++;
}

int comments_count(void) { return count; }

const Comment *comments_get(int index)
{
    if (index < 0 || index >= count) return NULL;
    return &items[index];
}

void comments_shutdown(void)
{
    comments_reset();
    free(items);
    items = NULL;
    cap = 0;
}
