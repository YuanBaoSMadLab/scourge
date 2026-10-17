// AOT lang.* → @sg_* 运行时符号降级表（单一事实源）。
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace scourge {
namespace aot {

// 类型码：i=i64  f=f64  I=i32  b=i1  p=!llvm.ptr；返回码同 + 'v'=无返回。
struct OpLower {
    const char* lang;   ///< 全名 "lang.<mnemonic>"
    const char* sg;     ///< 运行时符号（不含 @）
    const char* args;   ///< lang 层形参类型码
    const char* externArgs; ///< sg_* 完整 ABI 类型码
    char ret;           ///< 返回类型码
    bool hasCtx;        ///< 调用是否追加 panic 上下文 (fn, file, line)
    const char* cap = nullptr; ///< 能力域名（Capability.hpp；需授权行非空，
                               ///< LangCallRewrite 前置 sg_cap_check 查位）
};

llvm::ArrayRef<OpLower> langLowerings();

const OpLower* findLowering(llvm::StringRef name);

} // namespace aot
} // namespace scourge
