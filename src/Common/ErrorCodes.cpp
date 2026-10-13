// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 错误码表实现
// ============================================================================
#include "scourge/Common/ErrorCodes.hpp"

#include <cstdio>

namespace scourge {

std::string_view errcCode(Errc e) {
    // 警告码存储值 = 10000 + 显示编号（见 ErrorCodes.hpp 编码规则）。
    static thread_local char buf[8];
    uint16_t v = static_cast<uint16_t>(e);
    if (v >= 10000) {
        std::snprintf(buf, sizeof(buf), "W%u", static_cast<unsigned>(v - 10000));
    } else {
        std::snprintf(buf, sizeof(buf), "E%u", static_cast<unsigned>(v));
    }
    return buf;
}

std::string_view errcCategory(Errc e) {
    uint16_t v = static_cast<uint16_t>(e);
    if (v >= 10000) v -= 10000; // 警告归属其显示族
    if (v < 2000) return "syntax";
    if (v < 3000) return "type";
    if (v < 4000) return "contract";
    if (v < 5000) return "memory";
    return "external";
}

} // namespace scourge
