/* src/util.h — вспомогательные функции работы с памятью и строками.
 *
 * Все выделения памяти в трансляторе идут через эти обёртки: нехватка
 * памяти считается фатальной ошибкой и обрабатывается в одном месте,
 * поэтому остальной код не загромождён проверками на NULL.
 */
#ifndef C2PY_UTIL_H
#define C2PY_UTIL_H

#include <stddef.h>

void *xmalloc(size_t size);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *ptr, size_t size);

/* Копия строки; для NULL возвращает NULL. */
char *xstrdup(const char *s);

/* Копия не более n первых символов, всегда завершённая нулём. */
char *xstrndup(const char *s, size_t n);

#endif /* C2PY_UTIL_H */
