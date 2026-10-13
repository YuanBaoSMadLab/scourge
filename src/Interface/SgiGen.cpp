// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 编译器 — .sgi 生成器实现
// ============================================================================
#include "scourge/Interface/SgiGen.hpp"

#include <set>

namespace scourge {

namespace {

/// 契约子句原文（保留书写形式；子句表达式在源码中已规范化为单行）。
std::string clauseRawText(const SourceMgr& src, FileId file,
                          const SourceRange& r) {
    const std::string& text = src.file(file).text;
    if (r.offset + r.length <= text.size())
        return text.substr(r.offset, r.length);
    return "<不可恢复>";
}

} // namespace

std::string generateSgi(const SourceMgr& src, const Module& m,
                        const ModuleSymbols& syms) {
    std::string out;
    out += "# scourge interface v1（scourge interface 工具生成，请勿手改）\n";
    out += "# 模块: " + m.name + "\n";
    out += "# 本文件是接口契约：依赖方只看这里，无需阅读实现。\n\n";
    out += "module " + m.name + "\n\n";

    // import 段（源码顺序去重）
    std::set<std::string> seen;
    for (const auto& s : m.body) {
        if (s->kind != Stmt::Kind::Import) continue;
        auto& im = static_cast<const Import&>(*s);
        if (seen.insert(im.module).second) out += "import " + im.module + "\n";
    }
    if (!seen.empty()) out += "\n";

    for (const auto& s : m.body) {
        switch (s->kind) {
        case Stmt::Kind::Assign: {
            auto& a = static_cast<const Assign&>(*s);
            if (a.target->kind != Expr::Kind::Name || !a.ann) break;
            auto& n = static_cast<const Name&>(*a.target);
            auto it = syms.globals.find(n.name);
            if (it == syms.globals.end()) break;
            out += n.name + ": " + typeToString(it->second) + "\n";
            break;
        }
        case Stmt::Kind::FuncDef: {
            auto& f = static_cast<const FuncDef&>(*s);
            auto it = syms.funcs.find(f.name);
            if (it == syms.funcs.end()) break;
            const FuncSig& sig = it->second;

            // 分隔空行：已处于空行后（import 段/文件头）则不重复。
            bool atBlank = out.size() >= 2 &&
                           out[out.size() - 1] == '\n' &&
                           out[out.size() - 2] == '\n';
            if (!out.empty() && !atBlank) out += "\n";
            if (sig.isPure) out += "@pure\n";
            // @extern 与 async 改变调用约定，属于接口契约的一部分。
            for (const auto& d : f.decorators) {
                if (d.name != "extern") continue;
                std::string lang = "c";
                if (!d.args.empty() && d.args[0]->kind == Expr::Kind::StrLit)
                    lang = static_cast<const StrLit*>(d.args[0].get())->strValue;
                out += "@extern(\"" + lang + "\")\n";
            }
            out += f.isAsync ? "async def " : "def ";
            out += f.name + "(";
            for (size_t i = 0; i < sig.params.size(); ++i) {
                if (i) out += ", ";
                out += f.params[i].name + ": " + typeToString(sig.params[i]);
                if (f.params[i].defaultValue)
                    out += " = <default>"; // 默认值保留语义位置，值以源码为准
            }
            out += ") -> " + typeToString(sig.ret) + ":\n";

            if (!f.docstring.empty()) {
                // 仅保留描述行；契约行以 canonical 形式在下方输出。
                std::string desc;
                size_t pos = 0;
                while (pos < f.docstring.size()) {
                    size_t eol = f.docstring.find('\n', pos);
                    std::string line = f.docstring.substr(
                        pos, (eol == std::string::npos ? f.docstring.size() : eol) - pos);
                    pos = (eol == std::string::npos) ? f.docstring.size() : eol + 1;
                    size_t b = line.find_first_not_of(" \t\r");
                    std::string t =
                        b == std::string::npos ? "" : line.substr(b);
                    if (t.rfind("requires:", 0) == 0 ||
                        t.rfind("ensures:", 0) == 0 ||
                        t.rfind("invariant:", 0) == 0)
                        continue;
                    if (!desc.empty()) desc += "\n    ";
                    desc += t;
                }
                // 去除尾部空白后判空。
                size_t last = desc.find_last_not_of(" \t\n");
                if (last != std::string::npos) {
                    desc = desc.substr(0, last + 1);
                    out += "    \"\"\"" + desc + "\"\"\"\n";
                }
            }
            for (const auto& c : f.contracts) {
                const char* kw = c.kind == ContractClause::Kind::Requires
                                     ? "requires"
                                     : (c.kind == ContractClause::Kind::Ensures
                                            ? "ensures"
                                            : "invariant");
                out += "    " + std::string(kw) + ": " +
                       clauseRawText(src, m.file, c.range) + "\n";
            }
            break;
        }
        default:
            break;
        }
    }
    return out;
}

} // namespace scourge
