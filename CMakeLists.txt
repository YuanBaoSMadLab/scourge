# Copyright (c) 2026 Scourge Project. All rights reserved.
cmake_minimum_required(VERSION 3.28)

project(scourge
    VERSION 0.5.0
    DESCRIPTION "Scourge：为 AI 代理时代设计的确定性验证型语言（P2 外部协作接口）"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认 Release 单配置； Ninja 用户可用 -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# 编译选项：MSVC(cl/clang-cl) 与 GNU 风格(clang++/g++) 双通道
# ---------------------------------------------------------------------------
set(SCOURGE_GNU_FLAGS -Wall -Wextra -Wpedantic -Wconversion)
set(SCOURGE_MSVC_FLAGS /W4 /permissive- /utf-8 /EHsc)

if(MSVC)
    add_compile_options(${SCOURGE_MSVC_FLAGS})
else()
    add_compile_options(${SCOURGE_GNU_FLAGS})
endif()

# ---------------------------------------------------------------------------
# scourge_core：编译器核心静态库（Common / Syntax / Sema / Interface）
# ---------------------------------------------------------------------------
add_library(scourge_core STATIC
    src/Common/Source.cpp
    src/Common/Json.cpp
    src/Common/JsonWriter.cpp
    src/Common/ErrorCodes.cpp
    src/Common/Diagnostic.cpp
    src/Syntax/Lexer.cpp
    src/Syntax/Parser.cpp
    src/Sema/Type.cpp
    src/Sema/TypeChecker.cpp
    src/Interface/SgiGen.cpp
    src/BC/Compiler.cpp
    src/BC/VM.cpp
    src/BC/Serialize.cpp
    src/AOT/MLIRGen.cpp
    src/Tool/Session.cpp
    src/Tool/Runner.cpp
)
target_include_directories(scourge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
add_executable(scourge src/Tool/main.cpp src/Tool/Repl.cpp src/Tool/McpServer.cpp
    src/Tool/Builder.cpp)
target_link_libraries(scourge PRIVATE scourge_core)

add_executable(scourge_tests tests/test_runner.cpp src/Tool/Repl.cpp src/Tool/McpServer.cpp)
target_link_libraries(scourge_tests PRIVATE scourge_core)

enable_testing()
add_test(NAME scourge_tests COMMAND scourge_tests)
