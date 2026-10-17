@echo off
set "CLANG=%SCOURGE_CLANG%"
if "%CLANG%"=="" set "CLANG=D:\LLVM\bin\clang++.exe"
if not exist "%CLANG%" set "CLANG=clang++"
if not exist "..\bin" mkdir "..\bin"
"%CLANG%" -shared -O2 -std=c++17 -I"..\." shim.cpp -o "..\bin\demo_cpp_shim.dll"
if errorlevel 1 exit /b 1
echo bindgen: 已生成 ..\bin\demo_cpp_shim.dll
