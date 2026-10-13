// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — 诊断引擎（设计文档第三章/第六章/十二章第 5 条）
// ----------------------------------------------------------------------------
//  核心纪律：JSON（机器）与人类可读文本由同一 Diagnostic 结构生成，
//  两通道永不失同步。错误码来自 ErrorCodes.hpp 稳定表。
//
//  Diagnostic 携带：
//    code/severity/message/range/fixits/notes
//  fixit 由编译器规则生成（非模型生成），永远可信（第六章第 2 条）。
// ============================================================================
#pragma once

#include "scourge/Common/ErrorCodes.hpp"
#include "scourge/Common/JsonWriter.hpp"
#include "scourge/Common/Source.hpp"

#include <string>
#include <vector>

namespace scourge {

enum class Severity : uint8_t { Error, Warning, Note, Fatal };

/// 确定性修复建议：把 range 内文本替换为 replacement。
struct FixIt {
    SourceRange range;
    std::string replacement;
    std::string message; ///< 说明为什么这样改（简短）
};

/// 一条诊断。notes 是附加说明（如 “此处第一次定义”）。
struct Diagnostic {
    Errc code;
    Severity severity;
    std::string message;
    FileId file = kInvalidFile;
    SourceRange range;
    std::vector<FixIt> fixits;
    std::vector<Diagnostic> notes;
};

/// 诊断收集与双通道输出。
class DiagEngine {
public:
    explicit DiagEngine(const SourceMgr& src) : src_(src) {}

    /// 组装并登记一条诊断（notes 由调用方先填充）。
    Diagnostic& report(Errc code, Severity sev, FileId file, SourceRange range,
                       std::string message);

    /// 便捷重载：Error 级。
    void error(Errc code, FileId file, SourceRange range, std::string message) {
        report(code, Severity::Error, file, range, std::move(message));
    }
    void warning(Errc code, FileId file, SourceRange range, std::string message) {
        report(code, Severity::Warning, file, range, std::move(message));
    }

    /// 给最近一条诊断追加 fixit / note。
    void attachFixIt(FixIt fix);
    void attachNote(Diagnostic note);

    /// 并入一条来自其它（如并行工作线程）引擎的成品诊断。
    /// 用于分层并行检查：各模块在独立引擎上产出诊断后合并回主引擎，
    /// 合并顺序由调用方按确定序控制，保证双通道输出不随并发度漂移。
    void adopt(const Diagnostic& d);

    // ---- 结果查询 -----------------------------------------------------------
    const std::vector<Diagnostic>& all() const { return diags_; }
    size_t errorCount() const { return errorCount_; }
    size_t warningCount() const { return warningCount_; }
    bool hasErrors() const { return errorCount_ > 0; }
    bool limitReached() const { return limitReached_; }

    // ---- 双通道输出 -----------------------------------------------------------
    /// 机器通道：JSON 数组。
    void emitJson(JsonWriter& w) const;
    /// 人类通道：带源码引用与插入符的文本。
    void emitText(std::string& out) const;

private:
    static std::string_view severityName(Severity s);

    const SourceMgr& src_;
    std::vector<Diagnostic> diags_;
    size_t errorCount_ = 0;
    size_t warningCount_ = 0;
    bool limitReached_ = false;
    Diagnostic sink_; ///< 上限后丢弃错误的返回占位（保持签名 Diagnostic&）

    /// 错误上限：AI 修复循环需要完整错误面，但也要防病态输入拖垮代理。
    static constexpr size_t kMaxErrors = 100;
};

} // namespace scourge
