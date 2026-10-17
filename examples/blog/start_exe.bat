@echo off
rem AOT 模式启动：运行编译好的单文件 exe
cd /d %~dp0
if not exist blog_server.exe (
    echo [build] blog_server.exe 不存在，先编译...
    D:\Scourge\build\scourge.exe build main.sg -o blog_server.exe || goto :err
)
blog_server.exe
pause
exit /b 0
:err
echo 编译失败
pause
exit /b 1
