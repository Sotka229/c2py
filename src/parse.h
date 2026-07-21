/* src/parse.h — интерфейс синтаксического анализатора. */
#ifndef C2PY_PARSE_H
#define C2PY_PARSE_H

#include <stdio.h>

#include "ast.h"

/* Строят дерево программы. Возвращают NULL, если найдены ошибки:
 * при ошибке дерево заведомо неполное, и работать с ним нельзя.
 * Подробности всегда доступны через модуль diag. */
Program *parse_string(const char *src);
Program *parse_file(FILE *fp);

#endif /* C2PY_PARSE_H */
