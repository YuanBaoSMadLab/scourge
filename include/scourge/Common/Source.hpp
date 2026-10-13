// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — SourceMgr：源码缓冲与位置管理
// ----------------------------------------------------------------------------
//  设计要点（对应设计文档第二章/十二章）：
//  * 所有诊断都携带 (FileId, SourceRange)，由诊断引擎统一渲染，
//    JSON 与人类可读文本共用同一结构，永不失同步。
//  * 源码以 UTF-8 文本持有；行列换算按“字节偏移 → 行/列”进行，
//    列号按 UTF-8 码点计数（对 AI 代理足够确定，且与常见编辑器一致）。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scourge {

/// 源内区间：起始于字节偏移 offset，长度 length 字节。
struct SourceRange {
    uint32_t offset = 0;
    uint32_t length = 0;

    SourceRange() = default;
    SourceRange(uint32_t off, uint32_t len) : offset(off), length(len) {}

    /// 覆盖 a 到 b 的最小区间（要求同一文件内且 a.end() <= b.begin()）。
    static SourceRange merge(const SourceRange& a, const SourceRange& b) {
        if (b.length == 0) return a;
        if (a.length == 0) return b;
        uint32_t aEnd = a.offset + a.length;
        uint32_t bEnd = b.offset + b.length;
        uint32_t beg = a.offset < b.offset ? a.offset : b.offset;
        uint32_t end = aEnd > bEnd ? aEnd : bEnd;
        return SourceRange(beg, end - beg);
    }
};

/// 文件编号；kInvalidFile 表示“无关联文件”。
using FileId = uint32_t;
inline constexpr FileId kInvalidFile = 0xFFFFFFFFu;

/// 行列号（1 基）。
struct LineCol {
    uint32_t line = 0;
    uint32_t col = 0;
};

/// 一个已加载的源文件（只读）。
struct SourceFile {
    std::string path;    ///< 展示用路径（相对或绝对，由调用方决定）
    std::string text;    ///< 完整 UTF-8 文本
};

/// SourceMgr 拥有全部源文件；FileId 即内部下标（从 0 开始）。
class SourceMgr {
public:
    /// 载入一个源文件（不读盘，调用方负责填充内容）。
    /// 统一剥离 UTF-8 BOM（编辑器常见产物，不参与词法）。
    /// 返回新 FileId。
    FileId addFile(std::string path, std::string text) {
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF) {
            text.erase(0, 3);
        }
        files.push_back(
            std::make_unique<SourceFile>(SourceFile{std::move(path), std::move(text)}));
        return static_cast<FileId>(files.size() - 1);
    }

    const SourceFile& file(FileId id) const { return *files[id]; }
    size_t fileCount() const { return files.size(); }

    /// 字节偏移 → (行, 列)。列按 UTF-8 码点计数，1 基。
    LineCol lineCol(FileId id, uint32_t offset) const;

    /// 取第 line 行（不含行尾换行符）。
    std::string lineText(FileId id, uint32_t line) const;

private:
    std::vector<std::unique_ptr<SourceFile>> files;
};

} // namespace scourge
