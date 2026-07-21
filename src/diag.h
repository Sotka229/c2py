/* src/diag.h — накопление и вывод диагностических сообщений.
 *
 * Сообщения выводятся в формате "файл:строка:столбец: error: текст",
 * привычном для gcc, поэтому редакторы и среды разработки умеют
 * разбирать его автоматически.
 *
 * Тексты сообщений на английском языке: консоль Windows работает
 * в однобайтовой кодировке и не отображает UTF-8.
 */
#ifndef C2PY_DIAG_H
#define C2PY_DIAG_H

#include "srcpos.h"

/* После этого числа сообщения перестают печататься. */
#define DIAG_MAX_REPORTED 20

void diag_init(const char *filename);
void diag_shutdown(void);

#if defined(__GNUC__)
#define C2PY_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define C2PY_PRINTF(a, b)
#endif

void diag_error_at(SrcPos pos, const char *fmt, ...) C2PY_PRINTF(2, 3);
void diag_warning_at(SrcPos pos, const char *fmt, ...) C2PY_PRINTF(2, 3);

int diag_error_count(void);
int diag_warning_count(void);
int diag_has_errors(void);

#endif /* C2PY_DIAG_H */
