// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — JsonWriter：零依赖 JSON 序列化
// ----------------------------------------------------------------------------
//  诊断 JSON（设计文档第六章：JSON 诊断一等公民）由此输出。
//  只做转义与结构拼装，不做解析——编译器只需生成端。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scourge {

class JsonWriter {
public:
    /// pretty=true 时输出带缩进的可读格式；false 时输出单行紧凑格式。
    explicit JsonWriter(bool pretty = false) : pretty_(pretty) {}

    // ---- 结构控制 -----------------------------------------------------------
    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    /// 写入对象键（随后应写值）。
    void key(std::string_view k);

    // ---- 值 -----------------------------------------------------------------
    void valueString(std::string_view s);
    void valueInt(int64_t v);
    void valueBool(bool b);
    void valueNull();

    /// key + 字符串值的组合便捷方法。
    void pairString(std::string_view k, std::string_view s);
    void pairInt(std::string_view k, int64_t v);
    void pairBool(std::string_view k, bool b);

    /// 取出结果（move 语义，调用后 writer 应弃用）。
    std::string take() { return std::move(out_); }

private:
    void writeIndent();
    void writeSep();           ///< 在容器成员之间补逗号
    void writeEscaped(std::string_view s);

    std::string out_;
    bool pretty_ = false;
    int depth_ = 0;
    /// 每层容器“已完成成员数”标记，栈顶为当前容器。
    std::vector<int> counts_;
    /// 刚写入对象键、值尚未写入——紧随的值/容器开始不补逗号
    ///（成员分隔逗号只在「键之前」由 writeSep 决定）。
    bool pendingKey_ = false;
};

} // namespace scourge
