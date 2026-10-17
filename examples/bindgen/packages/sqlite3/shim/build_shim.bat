@echo off
set "CLANG=%SCOURGE_CLANG%"
if "%CLANG%"=="" set "CLANG=D:\LLVM\bin\clang++.exe"
if not exist "%CLANG%" set "CLANG=clang++"
set "CLANGC=%SCOURGE_CLANG_C%"
if "%CLANGC%"=="" set "CLANGC=D:\LLVM\bin\clang.exe"
if not exist "%CLANGC%" set "CLANGC=clang"
if not exist "..\bin" mkdir "..\bin"
"%CLANGC%" -O2 -I"..\." -c "..\sqlite3.c" -o "sqlite3.obj"
if errorlevel 1 exit /b 1
"%CLANG%" -shared -O2 -std=c++17 -I"..\." shim.cpp "sqlite3.obj" -o "..\bin\sqlite3_shim.dll"
if errorlevel 1 exit /b 1
echo bindgen: 已生成 ..\bin\sqlite3_shim.dll
