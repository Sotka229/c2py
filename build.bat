@echo off
rem build.bat — сборка транслятора в Windows.
rem
rem Требуются flex, bison, gcc и python в переменной PATH.
rem
rem   build.bat          собрать транслятор
rem   build.bat clean    очистить каталог build

setlocal
cd /d "%~dp0"
python tools\build.py %*
exit /b %ERRORLEVEL%
