/* src/comments.h — таблица комментариев исходного файла.
 *
 * Комментарии не являются лексемами и не попадают в дерево, но их
 * желательно сохранить в результате трансляции. Лексер складывает их
 * сюда вместе с номером строки, а генератор кода позднее вставляет
 * их в подходящие места.
 *
 * Именно так решается ошибка исходного варианта: там комментарии
 * печатались прямо из действий лексера и попадали в вывод раньше
 * или позже нужного места, поскольку анализатор читает лексемы
 * с опережением на одну.
 */
#ifndef C2PY_COMMENTS_H
#define C2PY_COMMENTS_H

typedef struct {
    int line;     /* строка, где комментарий начинается           */
    int end_line; /* строка, где он заканчивается                 */
    int col;      /* столбец начала                               */
    int own_line; /* 1 — комментарий занимает строку целиком      */
    char *text;   /* содержимое без ограничителей                 */
} Comment;

void comments_reset(void);
void comments_add(int line, int end_line, int col, int own_line,
                  const char *text);
int  comments_count(void);
const Comment *comments_get(int index);
void comments_shutdown(void);

#endif /* C2PY_COMMENTS_H */
