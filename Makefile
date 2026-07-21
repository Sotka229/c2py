# Makefile — сборка транслятора c2py.
#
# Основной способ сборки:
#     python tools/build.py
#
# Этот файл повторяет ту же последовательность средствами make.
# Он рассчитан на GNU Make 3.80, поэтому в нём нет условных функций
# и других поздних возможностей.

CC       = gcc
CFLAGS   = -std=c99 -Wall -Wextra -Wno-unused-parameter -O2
GENFLAGS = -std=c99 -w -O2
INCLUDES = -Isrc -Ibuild

BUILD = build
OBJ   = $(BUILD)/obj

# Порядок важен: лексер использует заголовок, который создаёт bison.
GENERATED = $(BUILD)/parser.tab.c $(BUILD)/lex.yy.c

OBJECTS = \
	$(OBJ)/util.o     \
	$(OBJ)/strbuf.o   \
	$(OBJ)/diag.o     \
	$(OBJ)/types.o    \
	$(OBJ)/ast.o      \
	$(OBJ)/escape.o   \
	$(OBJ)/comments.o \
	$(OBJ)/format.o   \
	$(OBJ)/symtab.o   \
	$(OBJ)/sema.o     \
	$(OBJ)/codegen.o  \
	$(OBJ)/parser.tab.o \
	$(OBJ)/lex.yy.o

TARGET = $(BUILD)/c2py.exe

all: $(TARGET)

$(TARGET): $(OBJECTS) $(OBJ)/main.o
	$(CC) $(OBJECTS) $(OBJ)/main.o -o $@

# --- порождаемые анализаторы ---

$(BUILD)/parser.tab.c: src/parser.y
	-@mkdir $(BUILD)
	bison -d -o $(BUILD)/parser.tab.c src/parser.y

# flex версии 2.5.4 понимает только слитную форму -oФАЙЛ
$(BUILD)/lex.yy.c: src/lexer.l $(BUILD)/parser.tab.c
	flex -obuild/lex.yy.c src/lexer.l

# --- объектные файлы ---

$(OBJ)/%.o: src/%.c $(BUILD)/parser.tab.c
	-@mkdir $(BUILD)
	-@mkdir $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ)/parser.tab.o: $(BUILD)/parser.tab.c
	$(CC) $(GENFLAGS) $(INCLUDES) -c $(BUILD)/parser.tab.c -o $@

$(OBJ)/lex.yy.o: $(BUILD)/lex.yy.c
	$(CC) $(GENFLAGS) $(INCLUDES) -c $(BUILD)/lex.yy.c -o $@

# --- очистка ---

clean:
	python tools/build.py clean

.PHONY: all clean
