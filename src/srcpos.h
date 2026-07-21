/* src/srcpos.h — позиция в исходном тексте.
 *
 * Вынесена в отдельный заголовок, чтобы лексер и подсистема диагностики
 * не зависели от всего описания синтаксического дерева.
 */
#ifndef C2PY_SRCPOS_H
#define C2PY_SRCPOS_H

typedef struct {
    int line; /* нумерация с единицы */
    int col;  /* нумерация с единицы */
} SrcPos;

#endif /* C2PY_SRCPOS_H */
