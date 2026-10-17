// ============================================================================
//  Scourge 编译器 — Builder 内部共享工具（Builder.cpp / BuilderBare.cpp 共用）
// ----------------------------------------------------------------------------
//  职责边界：外部命令捕获、C 字符串字面量转义、工具链指纹。从 Builder.cpp
//  匿名 namespace 提升为内部共享头——裸机与宿主两条构建路径使用同一实现
//  （单一事实源，禁止平行拷贝）。仅供 src/Tool/ 两个 BuilderTU include。
// ============================================================================
#pragma once

#define _CRT_SECURE_NO_WARNINGS // std::getenv（MSVC 弃用告警；单线程读取环境）

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace scourge {
namespace bshared {

/// 执行外部命令并捕获合并输出（stdout+stderr → 捕获文件 → 读回 → 删除）。
inline int runCapture(const std::string& exe, const std::string& args,
                      const fs::path& capPath, std::string& out) {
    std::error_code ec;
    fs::remove(capPath, ec);
#ifdef _WIN32
    // Windows 命令解释器对以引号开头且含特殊字符的命令会剥首尾引号——
    // 外层再包一对引号（官方文档的标准规避法），内层各路径引号原样保留。
    std::string cmdline = "\"\"" + exe + "\" " + args + " > \"" +
                          capPath.string() + "\" 2>&1\"";
#else
    // POSIX: /bin/sh 直跑（双引号语义一致，无需外层规避对）。
    std::string cmdline = "\"" + exe + "\" " + args + " > '" +
                          capPath.string() + "' 2>&1";
#endif
    int rc = std::system(cmdline.c_str());
    out.clear();
    std::ifstream in(capPath, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
    }
    fs::remove(capPath, ec);
    return rc;
}

/// 内嵌文本 → C 字符串字面量体（换行 → "\n" 续段，转义反斜杠与引号）。
inline std::string cStringLiteral(const char* raw) {
    std::string out;
    for (const char* p = raw; *p; ++p) {
        switch (*p) {
        case '\n': out += "\\n\"\n\""; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out += *p; break;
        }
    }
    return out;
}

/// 工具链指纹：clang 路径 + 大小 + 修改时间——工具链升级即缓存失效。
inline std::string clangTag(const fs::path& clang) {
    std::error_code ec;
    uintmax_t size = fs::file_size(clang, ec);
    uint64_t sizeV = ec ? 0 : static_cast<uint64_t>(size);
    ec.clear();
    auto t = fs::last_write_time(clang, ec);
    uint64_t mtime = ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
    return clang.string() + "|" + std::to_string(sizeV) + "|" +
           std::to_string(mtime);
}

/// LLVM 工具定位（SCOURGE_LLVM_BIN 可覆盖 → 默认 D:/LLVM/bin → PATH 回退）。
/// 返回 bin 目录；找不到返回空路径。
inline fs::path llvmBinDir() {
    const char* envBin = std::getenv("SCOURGE_LLVM_BIN");
    fs::path dir = (envBin && *envBin)
                       ? fs::path(envBin)
                       : fs::path(
#ifdef _WIN32
                             "D:/LLVM/bin"
#else
                             "/usr/bin"
#endif
                         );
    std::error_code ec;
    if (fs::exists(dir, ec)) return dir;
    // PATH 回退（Linux 宿主常规途径）。
    const char* pe = std::getenv("PATH");
    if (!pe) return {};
    std::string p = pe;
    std::string delim =
#ifdef _WIN32
        ";";
#else
        ":";
#endif
    size_t st = 0;
    while (st <= p.size()) {
        size_t en = p.find(delim, st);
        std::string seg =
            p.substr(st, en == std::string::npos ? std::string::npos : en - st);
        fs::path cand = fs::path(seg);
        if (fs::exists(cand /
#ifdef _WIN32
                           "clang.exe"
#else
                           "clang"
#endif
                       ,
                       ec))
            return cand;
        if (en == std::string::npos) break;
        st = en + 1;
    }
    return {};
}

} // namespace bshared
} // namespace scourge
