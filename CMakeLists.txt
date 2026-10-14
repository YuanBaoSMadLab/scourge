# Copyright (c) 2026 Scourge Project. All rights reserved.
cmake_minimum_required(VERSION 3.28)

project(scourge
    VERSION 0.5.0
    DESCRIPTION "Scourge：为 AI 代理时代设计的确定性验证型语言（P2 外部协作接口）"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 统一动态 CRT（/MD）：E:\llvm 的 LLVM/MLIR 库为 VS Release 默认动态 CRT 构建，
# 全工程对齐避免混链冲突（LNK4217/4286 → LNK1169；find_package 后会再次重设）。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

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
    src/Tool/Builder.cpp src/AOT/InProcess.cpp src/AOT/LangDialect.cpp
    src/AOT/LangToAffine.cpp src/AOT/LangToLLVM.cpp src/AOT/LangToLinalg.cpp)
target_link_libraries(scourge PRIVATE scourge_core)

# ---- 进程内 MLIR/LLVM（build 命令阶段 2+3 内联，摆脱 mlir-opt/mlir-translate）----
# 库源：E:\llvm 自建树（llvm+mlir，Release，断言 ON → LLVM_ENABLE_ABI_BREAKING_CHECKS=1，
# 全树 ABI 自洽；SCOURGE_LLVM_ROOT 可覆盖）。该树构建时未接入 zlib/zstd（缓存
# NOTFOUND，压缩路径整体关闭），无第三方压缩符号依赖。clang 链接器仍取自
# D:\LLVM\bin（仅作最终驱动，与库版本无耦合）。
if(NOT SCOURGE_LLVM_ROOT)
    set(SCOURGE_LLVM_ROOT "E:/llvm/build")
endif()
# LLVMDebugInfoPDB 的链接接口引用 DIASDK::Diaguids（本工程不用 PDB，空接口目标
# 仅满足配置期存在性检查）。
if(NOT TARGET DIASDK::Diaguids)
    add_library(DIASDK::Diaguids INTERFACE IMPORTED)
endif()
find_package(LLVM REQUIRED CONFIG HINTS "${SCOURGE_LLVM_ROOT}/lib/cmake/llvm")
find_package(MLIR REQUIRED CONFIG HINTS "${SCOURGE_LLVM_ROOT}/lib/cmake/mlir")
# LLVMConfig.cmake 会把 CMAKE_MSVC_RUNTIME_LIBRARY 硬设为 MultiThreaded（静态 CRT），
# 但 E:\llvm 库本体为 VS Release 默认动态 CRT（/MD）——find_package 之后统一重设回
# 动态 CRT，保证先于/晚于 find_package 定义的所有目标与库一致（否则 LNK1169/LNK2038）。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(scourge SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
                                                  ${MLIR_INCLUDE_DIRS})
# ---------------------------------------------------------------------------
# Lang 方言 ODS 生成（AOT 步骤 2）：mlir-tblgen 取自 E:\llvm 同源树，
# 生成物只落 build 目录（严禁泄漏源码树）；LangOps.td 为唯一入库事实源。
# ---------------------------------------------------------------------------
set(SCOURGE_LANG_TD "${CMAKE_CURRENT_SOURCE_DIR}/include/scourge/AOT/LangOps.td")
set(SCOURGE_LANG_GEN "${CMAKE_CURRENT_BINARY_DIR}/lang_gen")
if(NOT SCOURGE_MLIR_TBLGEN)
    find_program(SCOURGE_MLIR_TBLGEN mlir-tblgen
        HINTS "${SCOURGE_LLVM_ROOT}/Release/bin" "${SCOURGE_LLVM_ROOT}/bin")
endif()
if(NOT SCOURGE_MLIR_TBLGEN)
    message(FATAL_ERROR "未找到 mlir-tblgen（Lang 方言 ODS 生成需要）："
        "请传 -DSCOURGE_MLIR_TBLGEN=<路径>，或确认 SCOURGE_LLVM_ROOT 下"
        " Release/bin 或 bin 中存在 mlir-tblgen")
endif()
set(SCOURGE_TBLGEN_ARGS)
foreach(_d ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
    list(APPEND SCOURGE_TBLGEN_ARGS -I "${_d}")
endforeach()
set(SCOURGE_LANG_GEN_OUTPUTS
    "${SCOURGE_LANG_GEN}/LangOps.h.inc"
    "${SCOURGE_LANG_GEN}/LangOps.cpp.inc"
    "${SCOURGE_LANG_GEN}/LangDialect.h.inc"
    "${SCOURGE_LANG_GEN}/LangDialect.cpp.inc")
file(MAKE_DIRECTORY "${SCOURGE_LANG_GEN}")
add_custom_command(
    OUTPUT ${SCOURGE_LANG_GEN_OUTPUTS}
    COMMAND "${SCOURGE_MLIR_TBLGEN}" -gen-dialect-decls "${SCOURGE_LANG_TD}" -o "${SCOURGE_LANG_GEN}/LangDialect.h.inc" ${SCOURGE_TBLGEN_ARGS}
    COMMAND "${SCOURGE_MLIR_TBLGEN}" -gen-dialect-defs "${SCOURGE_LANG_TD}" -o "${SCOURGE_LANG_GEN}/LangDialect.cpp.inc" ${SCOURGE_TBLGEN_ARGS}
    COMMAND "${SCOURGE_MLIR_TBLGEN}" -gen-op-decls "${SCOURGE_LANG_TD}" -o "${SCOURGE_LANG_GEN}/LangOps.h.inc" ${SCOURGE_TBLGEN_ARGS}
    COMMAND "${SCOURGE_MLIR_TBLGEN}" -gen-op-defs "${SCOURGE_LANG_TD}" -o "${SCOURGE_LANG_GEN}/LangOps.cpp.inc" ${SCOURGE_TBLGEN_ARGS}
    DEPENDS "${SCOURGE_LANG_TD}"
    COMMENT "scourge: mlir-tblgen 生成 Lang 方言（生成物仅落 build 目录）"
    VERBATIM)
add_custom_target(scourge_lang_gen DEPENDS ${SCOURGE_LANG_GEN_OUTPUTS})
add_dependencies(scourge scourge_lang_gen)
target_include_directories(scourge PRIVATE "${SCOURGE_LANG_GEN}")

target_link_libraries(scourge PRIVATE
    MLIRIR MLIRParser MLIRPass MLIRTransformUtils
    MLIRFuncDialect MLIRArithDialect MLIRControlFlowDialect MLIRLLVMDialect
    MLIRLinalgDialect MLIRMemRefDialect MLIRSCFDialect MLIRAffineDialect
    MLIRLinalgTransforms MLIRSCFToControlFlow MLIRAffineToStandard
    MLIRMemRefToLLVM MLIRIndexToLLVM
    MLIRConvertToLLVMPass MLIRTargetLLVM
    MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
    MLIRRegisterAllExtensions)

add_executable(scourge_tests tests/test_runner.cpp src/Tool/Repl.cpp src/Tool/McpServer.cpp)
target_link_libraries(scourge_tests PRIVATE scourge_core)

enable_testing()
add_test(NAME scourge_tests COMMAND scourge_tests)
