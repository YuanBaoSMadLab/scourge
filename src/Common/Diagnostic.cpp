// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 诊断引擎实现（JSON 与文本双通道同源渲染）
// ============================================================================
#include "scourge/Common/Diagnostic.hpp"

#include <cstdio>

namespace scourge {

Diagnostic& DiagEngine::report(Errc code, Severity sev, FileId file,
                               SourceRange range, std::string message) {
    // 错误上限生效后静默丢弃后续错误（fatal 提示已入列，告知先修已报问题）。
    if (limitReached_ && sev == Severity::Error) {
        sink_ = Diagnostic{};
        return sink_;
    }
    if (sev == Severity::Error) {
        ++errorCount_;
        if (errorCount_ >= kMaxErrors && !limitReached_) {
            limitReached_ = true;
            // 上限提示本身不算入 errorCount_（避免自我递增）。
            Diagnostic d;
            d.code = Errc::E1005_UnexpectedToken;
            d.severity = Severity::Fatal;
            d.message = "错误数已达上限 " + std::to_string(kMaxErrors) +
                        "，停止收集（先修复已报告的问题）";
            d.file = file;
            d.range = range;
            diags_.push_back(std::move(d));
            return diags_.back();
        }
    } else if (sev == Severity::Warning) {
        ++warningCount_;
    }
    Diagnostic d;
    d.code = code;
    d.severity = sev;
    d.message = std::move(message);
    d.file = file;
    d.range = range;
    diags_.push_back(std::move(d));
    return diags_.back();
}

void DiagEngine::attachFixIt(FixIt fix) {
    if (!diags_.empty()) diags_.back().fixits.push_back(std::move(fix));
}

void DiagEngine::attachNote(Diagnostic note) {
    if (!diags_.empty()) diags_.back().notes.push_back(std::move(note));
}

void DiagEngine::adopt(const Diagnostic& d) {
    if (d.severity == Severity::Error) ++errorCount_;
    else if (d.severity == Severity::Warning) ++warningCount_;
    diags_.push_back(d);
}

std::string_view DiagEngine::severityName(Severity s) {
    switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Note: return "note";
    case Severity::Fatal: return "fatal";
    }
    return "error";
}

// ----------------------------------------------------------------------------
// 机器通道：JSON 数组
// ----------------------------------------------------------------------------
void DiagEngine::emitJson(JsonWriter& w) const {
    w.beginArray();
    for (const auto& d : diags_) {
        w.beginObject();
        w.pairString("code", errcCode(d.code));
        w.pairString("severity", severityName(d.severity));
        w.pairString("category", errcCategory(d.code));
        w.pairString("message", d.message);
        if (d.file != kInvalidFile) {
            w.pairString("file", src_.file(d.file).path);
            LineCol beg = src_.lineCol(d.file, d.range.offset);
            w.pairInt("line", beg.line);
            w.pairInt("col", beg.col);
            LineCol end = src_.lineCol(d.file, d.range.offset + d.range.length);
            w.pairInt("end_line", end.line);
            w.pairInt("end_col", end.col);
        } else {
            w.pairString("file", "");
            w.pairInt("line", 0);
            w.pairInt("col", 0);
            w.pairInt("end_line", 0);
            w.pairInt("end_col", 0);
        }
        w.key("fixits");
        w.beginArray();
        for (const auto& f : d.fixits) {
            w.beginObject();
            LineCol beg = src_.lineCol(d.file, f.range.offset);
            LineCol end = src_.lineCol(d.file, f.range.offset + f.range.length);
            w.pairInt("line", beg.line);
            w.pairInt("col", beg.col);
            w.pairInt("end_line", end.line);
            w.pairInt("end_col", end.col);
            w.pairString("replacement", f.replacement);
            w.pairString("message", f.message);
            w.endObject();
        }
        w.endArray();
        w.key("notes");
        w.beginArray();
        for (const auto& n : d.notes) {
            w.beginObject();
            w.pairString("message", n.message);
            if (n.file != kInvalidFile) {
                w.pairString("file", src_.file(n.file).path);
                LineCol beg = src_.lineCol(n.file, n.range.offset);
                w.pairInt("line", beg.line);
                w.pairInt("col", beg.col);
            }
            w.endObject();
        }
        w.endArray();
        w.endObject();
    }
    w.endArray();
}

// ----------------------------------------------------------------------------
// 人类通道：file:line:col: severity[code]: message + 源码引用
// ----------------------------------------------------------------------------
void DiagEngine::emitText(std::string& out) const {
    char numbuf[32];
    for (const auto& d : diags_) {
        if (d.file != kInvalidFile) {
            LineCol lc = src_.lineCol(d.file, d.range.offset);
            out += src_.file(d.file).path;
            std::snprintf(numbuf, sizeof(numbuf), ":%u:%u: ", lc.line, lc.col);
            out += numbuf;
        } else {
            out += "<unknown>: ";
        }
        out += severityName(d.severity);
        out += "[";
        out += errcCode(d.code);
        out += "]: ";
        out += d.message;
        out += "\n";

        // 源码引用行（单行区间才展示插入符；跨行区间只给首行）。
        // Q13：超长源行截断展示——保留插入符列的可见窗口（列前 20 字节
        // 上下文），UTF-8 续字节边界对齐；JSON 通道不受影响（携带完整区间）。
        if (d.file != kInvalidFile && d.range.length > 0) {
            LineCol beg = src_.lineCol(d.file, d.range.offset);
            std::string line = src_.lineText(d.file, beg.line);
            constexpr size_t kMaxShow = 200; ///< 展示窗口上限（字节）
            constexpr size_t kHead = 160;    ///< 首选窗口宽度（插入符靠前时）
            size_t col0 = beg.col - 1;       // 0 基列号（字节）
            size_t start = 0, end = line.size();
            bool truncated = false;
            auto snapUtf8 = [&line](size_t p) {
                while (p > 0 &&
                       (static_cast<unsigned char>(line[p]) & 0xC0) == 0x80)
                    --p;
                return p;
            };
            if (line.size() > kMaxShow) {
                truncated = true;
                if (col0 < kHead) {
                    end = snapUtf8(kHead);
                } else {
                    start = snapUtf8(col0 - 20); // 插入符列前留上下文
                    end = snapUtf8(start + kMaxShow);
                }
                if (end <= start) { start = 0; end = kMaxShow; }
                line = line.substr(start, end - start);
            }
            // 行号 gutter 宽度对齐。
            std::snprintf(numbuf, sizeof(numbuf), "%u", beg.line);
            std::string gutterPad(numbuf);
            std::string prefix = start > 0 ? "…" : "";
            out += "  " + gutterPad + " | " + prefix + line +
                   (truncated ? " …（截断）" : "") + "\n";
            std::string pad((start > 0 ? 1 : 0) + (col0 >= start ? col0 - start : 0),
                            ' ');
            uint32_t mark = d.range.length;
            // 限制插入符长度到展示窗口内剩余部分（跨行时首行打省略号）。
            size_t room = end > col0 ? end - col0 : 1;
            if (mark > room) mark = static_cast<uint32_t>(room);
            if (mark > 200) mark = 200;
            out += "  " + std::string(gutterPad.size(), ' ') + " | " + pad + "^";
            if (mark > 1) out += std::string(mark - 1, '~');
            out += "\n";
        }
        for (const auto& f : d.fixits) {
            out += "  = fixit: " + f.message;
            if (!f.replacement.empty()) out += "（替换为: " + f.replacement + "）";
            out += "\n";
        }
        for (const auto& n : d.notes) {
            if (n.file != kInvalidFile) {
                LineCol lc = src_.lineCol(n.file, n.range.offset);
                out += "  = note: " + src_.file(n.file).path;
                std::snprintf(numbuf, sizeof(numbuf), ":%u:%u ", lc.line, lc.col);
                out += numbuf;
            } else {
                out += "  = note: ";
            }
            out += n.message + "\n";
        }
    }
}

} // namespace scourge
