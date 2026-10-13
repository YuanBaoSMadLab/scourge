// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — SourceMgr 实现
// ============================================================================
#include "scourge/Common/Source.hpp"

namespace scourge {

LineCol SourceMgr::lineCol(FileId id, uint32_t offset) const {
    const std::string& text = files[id]->text;
    LineCol lc{1, 1};
    uint32_t limit = offset < text.size() ? offset : static_cast<uint32_t>(text.size());
    for (uint32_t i = 0; i < limit; ++i) {
        if (text[i] == '\n') {
            ++lc.line;
            lc.col = 1;
        } else if (static_cast<unsigned char>(text[i]) < 0x80 || (text[i] & 0xC0) == 0xC0) {
            // ASCII 直接计数；UTF-8 多字节序列只在其首字节计一次码点。
            ++lc.col;
        }
    }
    return lc;
}

std::string SourceMgr::lineText(FileId id, uint32_t line) const {
    const std::string& text = files[id]->text;
    uint32_t cur = 1;
    size_t i = 0;
    while (i < text.size() && cur < line) {
        if (text[i] == '\n') ++cur;
        ++i;
    }
    std::string out;
    while (i < text.size() && text[i] != '\n') {
        if (text[i] != '\r') out.push_back(text[i]);
        ++i;
    }
    return out;
}

} // namespace scourge
