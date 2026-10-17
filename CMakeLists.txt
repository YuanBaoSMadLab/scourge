# Copyright (c) 2026 Scourge Project. All rights reserved.
cmake_minimum_required(VERSION 3.28)

project(scourge
    VERSION 0.9.1
    DESCRIPTION "Scourge：为 AI 代理时代设计的确定性验证型语言（P2 外部协作接口）"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 本工程纯传统 #include（无 import）；关闭 C++20 modules 依赖扫描
# （clang-scan-deps 每 TU 全量预处理 MLIR 巨头且并行度低，纯浪费）。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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
    # lld-link 替代 MSVC link.exe（39MB 产物单线程符号解析是增量构建大头；
    # lld-link 多线程，链接耗时数倍下降）。仅 Windows 宿主强制；Linux 上
    # 尊重发行版默认链接器（lld 非必装）。
    if(WIN32)
        add_link_options(-fuse-ld=lld)
    endif()
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
    src/Syntax/Parser.cpp src/Syntax/ParserStmt.cpp
    src/Syntax/ParserExpr.cpp src/Syntax/ParserType.cpp
    src/Sema/Type.cpp
    src/Sema/TypeChecker.cpp src/Sema/TypeCheckerSym.cpp
    src/Sema/TypeCheckerFunc.cpp src/Sema/TypeCheckerStmt.cpp
    src/Sema/TypeCheckerExpr.cpp src/Sema/TypeCheckerLit.cpp
    src/Sema/TypeCheckerCall.cpp src/Sema/TypeCheckerBuiltin.cpp
    src/Interface/SgiGen.cpp
    src/BC/Compiler.cpp src/BC/CompilerExpr.cpp src/BC/CompilerStmt.cpp
    src/BC/VM.cpp src/BC/VMBuiltin.cpp src/BC/VMDispatch.cpp
    src/BC/VMOps.cpp
    src/BC/VMMethod.cpp
    src/BC/VMNet.cpp
    src/BC/VMThread.cpp
    src/BC/VMSys.cpp
    src/BC/VMExtern.cpp
    src/BC/VMStd.cpp
    src/BC/VMJson.cpp
    src/BC/Serialize.cpp
    src/AOT/MLIRGen.cpp src/AOT/MLIRGenFunc.cpp src/AOT/MLIRGenExpr.cpp
    src/AOT/MLIRGenLit.cpp src/AOT/MLIRGenCall.cpp src/AOT/MLIRGenBuiltin.cpp
    src/AOT/MLIRGenBuiltinSys.cpp src/AOT/MLIRGenStmt.cpp
    src/Tool/Session.cpp
    src/Tool/Runner.cpp
)
target_include_directories(scourge_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# 可执行文件（AOT/MLIR TU 拆至 scourge_aot：它们独吃 MLIR 巨头，见下）
# ---------------------------------------------------------------------------
add_executable(scourge src/Tool/main.cpp src/Tool/Repl.cpp src/Tool/McpServer.cpp
    src/Tool/Builder.cpp src/Tool/TestRunner.cpp)
target_link_libraries(scourge PRIVATE scourge_core)
# POSIX 运行时（BuilderRTSys.hpp 的 @extern dlopen 路径）需要 dl；
# CMAKE_DL_LIBS 在 Windows 上为空，无需平台分支。
target_link_libraries(scourge PRIVATE ${CMAKE_DL_LIBS})
# VMNet.cpp（F/G 专项网络内建）需要 Winsock。
if(WIN32)
    target_link_libraries(scourge PRIVATE ws2_32)
endif()

# ---- 进程内 MLIR/LLVM（build 命令阶段 2+3 内联，摆脱 mlir-opt/mlir-translate）----
# 库源：E:\llvm 自建树（llvm+mlir，Release，断言 ON → LLVM_ENABLE_ABI_BREAKING_CHECKS=1，
# 全树 ABI 自洽；SCOURGE_LLVM_ROOT 可覆盖）。该树构建时未接入 zlib/zstd（缓存
# NOTFOUND，压缩路径整体关闭），无第三方压缩符号依赖。clang 链接器仍取自
# D:\LLVM\bin（仅作最终驱动，与库版本无耦合）。
if(NOT SCOURGE_LLVM_ROOT)
    if(WIN32)
        set(SCOURGE_LLVM_ROOT "E:/llvm/build")
    else()
        # Linux 宿主：常见发行版 LLVM 安装前缀；find_package 的 HINTS 未命中
        # 时仍会走系统路径搜索。
        set(SCOURGE_LLVM_ROOT "/usr/lib/llvm")
    endif()
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

# ---------------------------------------------------------------------------
# scourge_aot：独吃 MLIR 的 5 个 TU（实测 12.7s/12.3s/6.6s/6.5s/5.6s，是全量
# 构建绝对大头——MLIR 头每 TU 全量解析）。独立成库后挂预编译头：MLIR 巨头
# 只解析一次；不碰 MLIR 的 20+ TU（core/Tool）不受 PCH 注入影响。
# ---------------------------------------------------------------------------
add_library(scourge_aot STATIC
    src/AOT/InProcess.cpp src/AOT/LangDialect.cpp src/AOT/LangToAffine.cpp
    src/AOT/LangToLLVM.cpp src/AOT/LangToLinalg.cpp)
target_include_directories(scourge_aot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(scourge_aot SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
                                                      ${MLIR_INCLUDE_DIRS})
target_precompile_headers(scourge_aot PRIVATE
    <mlir/IR/BuiltinOps.h>
    <mlir/IR/BuiltinDialect.h>
    <mlir/IR/PatternMatch.h>
    <mlir/Pass/Pass.h>
    <mlir/Transforms/DialectConversion.h>
    <mlir/Dialect/Func/IR/FuncOps.h>
    <mlir/Dialect/Arith/IR/Arith.h>
    <mlir/Dialect/ControlFlow/IR/ControlFlow.h>
    <mlir/Dialect/ControlFlow/IR/ControlFlowOps.h>
    <mlir/Dialect/LLVMIR/LLVMDialect.h>
    <mlir/Dialect/Linalg/IR/Linalg.h>
    <mlir/Dialect/MemRef/IR/MemRef.h>
    <llvm/ADT/DenseMap.h>
    <llvm/ADT/SmallVector.h>)
# ---------------------------------------------------------------------------
# Lang 方言 ODS 生成（AOT 步骤 2）：mlir-tblgen 取自 E:\llvm 同源树，
# 生成物只落 build 目录（严禁泄漏源码树）；LangOps.td 为唯一入库事实源。
# ---------------------------------------------------------------------------
set(SCOURGE_LANG_TD "${CMAKE_CURRENT_SOURCE_DIR}/include/scourge/AOT/LangOps.td")
set(SCOURGE_LANG_GEN "${CMAKE_CURRENT_BINARY_DIR}/lang_gen")
if(NOT SCOURGE_MLIR_TBLGEN)
    find_program(SCOURGE_MLIR_TBLGEN mlir-tblgen
        HINTS "${SCOURGE_LLVM_ROOT}/Release/bin" "${SCOURGE_LLVM_ROOT}/bin"
              "/usr/bin")
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
add_dependencies(scourge_aot scourge_lang_gen)
target_include_directories(scourge_aot PRIVATE "${SCOURGE_LANG_GEN}")

target_link_libraries(scourge_aot PRIVATE
    MLIRIR MLIRParser MLIRPass MLIRTransformUtils
    MLIRFuncDialect MLIRArithDialect MLIRControlFlowDialect MLIRLLVMDialect
    MLIRLinalgDialect MLIRMemRefDialect MLIRSCFDialect MLIRAffineDialect
    MLIRLinalgTransforms MLIRSCFToControlFlow MLIRAffineToStandard
    MLIRMemRefToLLVM MLIRIndexToLLVM
    MLIRConvertToLLVMPass MLIRTargetLLVM
    MLIRLLVMToLLVMIRTranslation MLIRBuiltinToLLVMIRTranslation
    MLIRRegisterAllExtensions)
target_link_libraries(scourge PRIVATE scourge_aot)

add_executable(scourge_tests tests/test_runner.cpp src/Tool/Repl.cpp
    src/Tool/McpServer.cpp src/Tool/TestRunner.cpp)
target_link_libraries(scourge_tests PRIVATE scourge_core)
if(WIN32)
    target_link_libraries(scourge_tests PRIVATE ws2_32)
endif()

enable_testing()
add_test(NAME scourge_tests COMMAND scourge_tests)
