#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Сборка транслятора c2py.

Использование:
    python tools/build.py            собрать транслятор
    python tools/build.py clean      очистить каталог build/

Скрипт намеренно не зависит от make: GNU Make 3.80 в связке с cmd.exe
ведёт себя непредсказуемо на путях с не-ASCII символами.

Сообщения в консоль выводятся латиницей: консоль Windows работает
в однобайтовой кодировке и не отображает UTF-8.
"""
import os
import shutil
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
BUILD = os.path.join(ROOT, "build")
OBJ = os.path.join(BUILD, "obj")

EXE = os.path.join(BUILD, "c2py.exe")

# Строгие флаги для собственного кода; сгенерированный код собирается мягче,
# так как flex 2.5.4 и bison 2.4.1 порождают код с устаревшими конструкциями.
CFLAGS = ["-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-g", "-O2"]
GENFLAGS = ["-std=c99", "-w", "-g", "-O2"]
INCLUDES = ["-I" + SRC, "-I" + BUILD]

GENERATED = ("parser.tab.c", "lex.yy.c")


def sh(cmd, cwd=None):
    """Запускает команду, возвращает (код возврата, объединённый вывод)."""
    proc = subprocess.run(
        cmd,
        cwd=cwd or ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding="utf-8",
        errors="replace",
    )
    return proc.returncode, proc.stdout or ""


def rel(path):
    return os.path.relpath(path, ROOT).replace("\\", "/")


def fail(msg, output=""):
    print("ERROR: " + msg)
    if output.strip():
        print(output.rstrip())
    sys.exit(1)


def ensure_dirs():
    for directory in (BUILD, OBJ):
        os.makedirs(directory, exist_ok=True)


def generate():
    """Порождает синтаксический и лексический анализаторы."""
    rc, out = sh(["bison", "-d", "-o", "build/parser.tab.c", "src/parser.y"])
    if rc != 0:
        fail("bison failed on src/parser.y", out)
    if out.strip():
        print("bison: " + out.strip())

    # flex 2.5.4 понимает только слитную форму -oФАЙЛ
    rc, out = sh(["flex", "-obuild/lex.yy.c", "src/lexer.l"])
    if rc != 0:
        fail("flex failed on src/lexer.l", out)
    if out.strip():
        print("flex: " + out.strip())


def source_files():
    """Файлы транслятора, включая порождённые flex и bison."""
    files = [
        os.path.join(SRC, name)
        for name in sorted(os.listdir(SRC))
        if name.endswith(".c")
    ]
    for name in GENERATED:
        files.append(os.path.join(BUILD, name))
    return files


def compile_obj(path, flags):
    obj = os.path.join(OBJ, os.path.basename(path)[:-2] + ".o")
    rc, out = sh(["gcc"] + flags + INCLUDES + ["-c", path, "-o", obj])
    if rc != 0:
        fail("cannot compile " + rel(path), out)
    if out.strip():
        print(out.rstrip())
    return obj


def build():
    ensure_dirs()
    generate()

    objects = []
    for path in source_files():
        flags = GENFLAGS if os.path.basename(path) in GENERATED else CFLAGS
        objects.append(compile_obj(path, flags))

    rc, out = sh(["gcc"] + objects + ["-o", EXE])
    if rc != 0:
        fail("link failed", out)
    print("built: " + rel(EXE))


def clean():
    if os.path.isdir(BUILD):
        shutil.rmtree(BUILD)
    print("build/ cleaned")


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "build"

    if target == "clean":
        clean()
    elif target == "build":
        build()
    else:
        print("usage: build.py [build|clean]")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
