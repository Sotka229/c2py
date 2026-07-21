#include <stdio.h>

int main() {
    // Объявляем и инициализируем переменные
    int i, sum = 0, product = 1;
    int diff = 100, quotient = 100, remainder;

    // Простое условие
    if (sum == 0 && quotient > 0) {
        printf("Initial state: sum = 0, quotient > 0\n");
    }

    // Цикл for от 1 до 5, выполняющий все необходимые операции
    for (i = 1; i <= 5; i++) {
        sum += i;      // сложение
        diff -= i;     // вычитание
        product *= i;  // умножение
        quotient /= i; // деление
        remainder = quotient % i; // остаток от деления

        // Выводим промежуточные результаты
        printf("i: %d, sum: %d, diff: %d, product: %d, quotient: %d, remainder: %d\n",
               i, sum, diff, product, quotient, remainder);

        // Условие на четность/нечетность
        int check = i;
        while (check % 2 != 0) { // продолжаем, пока число нечетное
            printf("%d is odd (checked with while loop)\n", i);
            break; // выходим из цикла, чтобы не попасть в бесконечный цикл
        }

        if (i % 2 == 0) {
            printf("%d is even\n", i);
        }
    }

    // Финальные операции с присваиванием
    sum += 5;
    diff -= 3;
    product *= 2;
    quotient /= 2;

    // Выводим итоговые значения
    printf("Final sum: %d, Final diff: %d, Final product: %d, Final quotient: %d\n",
           sum, diff, product, quotient);

    return 0;
}
