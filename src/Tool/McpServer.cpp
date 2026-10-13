// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — MCP 服务器实现（stdio JSON-RPC 2.0）
// ----------------------------------------------------------------------------
//  传输形态：每行一个 JSON-RPC 报文（MCP stdio 传输约定），响应立即冲刷。
//  请求解析走 Common/Json（解析端），响应组装走 Common/JsonWriter（生成端）
//  ——两端对称，转义纪律单一。工具语义全部复用既有模块：
//    check / interface → Session（全量验证 + .sgi 生成）
//    run               → Runner::runOnce（与 CLI run 零分叉，含 .sgbc 缓存）
//    repl_eval         → Repl（持久会话，跨 tools/call 存活）
// ============================================================================
#include "scourge/Tool/McpServer.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "scourge/Common/Json.hpp"
#include "scourge/Common/JsonWriter.hpp"
#include "scourge/Common/Version.hpp"
#include "scourge/Tool/Repl.hpp"
#include "scourge/Tool/Runner.hpp"
#include "scourge/Tool/Session.hpp"

namespace fs = std::filesystem;

namespace scourge {

namespace {

/// MCP stdio 传输的协议版本（当前生态支持最广的一版）。
constexpr const char* kProtocolVersion = "2024-11-05";

/// JSON-RPC 2.0 标准错误码（协议层错误；工具执行错误走 result.isError）。
constexpr int kErrParse = -32700;
constexpr int kErrInvalidRequest = -32600;
constexpr int kErrMethodNotFound = -32601;
constexpr int kErrInvalidParams = -32602;

/// 工具执行结果：content 文本序列 + isError（MCP result 语义）。
struct ToolResult {
    std::vector<std::string> texts;
    bool isError = false;
};

/// 单次 tools/call 的分发结局：协议层错误（!ok）或工具结果。
struct CallOutcome {
    bool ok = true;
    int errCode = 0;
    std::string errMsg;
    ToolResult result;
};

/// 请求对象中的 id 指针；无 id / id 为 null 视为通知（不响应）。
const json::Value* requestId(const json::Value& req) {
    const json::Value* id = req.find("id");
    if (!id || id->isNull()) return nullptr;
    return id;
}

/// 字符串参数提取：存在且为字符串返回指针，否则 nullptr。
const json::Value* argStr(const json::Value& args, const char* key) {
    const json::Value* v = args.find(key);
    return (v && v->isString()) ? v : nullptr;
}

/// 布尔参数提取：存在且为 bool 返回其值，否则默认值。
bool argBool(const json::Value& args, const char* key, bool dflt) {
    const json::Value* v = args.find(key);
    return (v && v->isBool()) ? v->boolean : dflt;
}

// ---- inputSchema 组装助手（JsonWriter 生成端） ----------------------------

void writeStringProp(JsonWriter& w, const char* name, const char* desc) {
    w.key(name);
    w.beginObject();
    w.pairString("type", "string");
    w.pairString("description", desc);
    w.endObject();
}

void writeBoolProp(JsonWriter& w, const char* name, const char* desc) {
    w.key(name);
    w.beginObject();
    w.pairString("type", "boolean");
    w.pairString("description", desc);
    w.endObject();
}

void writeIntProp(JsonWriter& w, const char* name, const char* desc) {
    w.key(name);
    w.beginObject();
    w.pairString("type", "integer");
    w.pairString("description", desc);
    w.endObject();
}

/// 单个工具条目：name / description / inputSchema（object + properties + required）。
void writeTool(JsonWriter& w, const char* name, const char* desc,
               const std::function<void(JsonWriter&)>& fillProps,
               std::initializer_list<const char*> required) {
    w.beginObject();
    w.pairString("name", name);
    w.pairString("description", desc);
    w.key("inputSchema");
    w.beginObject();
    w.pairString("type", "object");
    w.key("properties");
    w.beginObject();
    fillProps(w);
    w.endObject();
    w.key("required");
    w.beginArray();
    for (const char* k : required) w.valueString(k);
    w.endArray();
    w.endObject();
    w.endObject();
}

} // namespace

// ----------------------------------------------------------------------------
// 协议状态与分发
// ----------------------------------------------------------------------------

struct McpServer::State {
    std::istream& in;
    std::ostream& out;
    std::ostringstream replOut;  ///< REPL 输出捕获（Repl 持有成员流引用）
    std::ostringstream replErr;  ///< REPL 诊断捕获（成员生命周期覆盖会话）
    std::unique_ptr<Repl> repl;  ///< MCP 生命周期内持久的 REPL 会话

    State(std::istream& i, std::ostream& o) : in(i), out(o) {}

    int run() {
        std::string line;
        while (std::getline(in, line)) {
            bool blank = true;
            for (char c : line)
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    blank = false;
                    break;
                }
            if (blank) continue;
            handleLine(line);
        }
        return 0;
    }

    // ---- 响应组装（紧凑单行 JSON + 立即冲刷） -------------------------------

    void writeId(JsonWriter& w, const json::Value* id) {
        w.key("id");
        if (!id) {
            w.valueNull();
            return;
        }
        switch (id->kind) {
        case json::Value::Kind::Int: w.valueInt(id->integer); break;
        case json::Value::Kind::String: w.valueString(id->str); break;
        default: w.valueNull(); break; // double/bool id：MVP 视为 null
        }
    }

    void respond(const json::Value* id,
                 const std::function<void(JsonWriter&)>& fillBody) {
        JsonWriter w(/*pretty=*/false);
        w.beginObject();
        w.pairString("jsonrpc", "2.0");
        writeId(w, id);
        fillBody(w);
        w.endObject();
        out << w.take() << "\n";
        out.flush();
    }

    void respondResult(const json::Value* id,
                       const std::function<void(JsonWriter&)>& fillResult) {
        respond(id, [&](JsonWriter& w) {
            w.key("result");
            fillResult(w);
        });
    }

    void respondError(const json::Value* id, int code,
                      const std::string& msg) {
        respond(id, [&](JsonWriter& w) {
            w.key("error");
            w.beginObject();
            w.pairInt("code", code);
            w.pairString("message", msg);
            w.endObject();
        });
    }

    // ---- 请求分发 ------------------------------------------------------------

    void handleLine(const std::string& line) {
        json::Value req;
        std::string err;
        if (!json::parse(line, req, err)) {
            respondError(nullptr, kErrParse, "请求不是合法 JSON：" + err);
            return;
        }
        const json::Value* method = req.find("method");
        if (!req.isObject() || !method || !method->isString()) {
            respondError(requestId(req), kErrInvalidRequest,
                         "请求缺少 method 字符串字段");
            return;
        }
        const json::Value* id = requestId(req);
        if (!id) return; // 通知：不响应（无状态握手，initialized 无需处理）

        const std::string& m = method->str;
        if (m == "initialize") {
            respondResult(id, [](JsonWriter& w) { writeInitialize(w); });
            return;
        }
        if (m == "ping") {
            respondResult(id,
                          [](JsonWriter& w) { w.beginObject(); w.endObject(); });
            return;
        }
        if (m == "tools/list") {
            respondResult(id, [](JsonWriter& w) { writeToolList(w); });
            return;
        }
        if (m == "tools/call") {
            static const json::Value kNoParams = [] {
                json::Value v;
                v.kind = json::Value::Kind::Object;
                return v;
            }();
            const json::Value* params = req.find("params");
            handleToolsCall(id, params && params->isObject() ? *params
                                                             : kNoParams);
            return;
        }
        respondError(id, kErrMethodNotFound, "未知方法 '" + m + "'");
    }

    void handleToolsCall(const json::Value* id, const json::Value& params) {
        const json::Value* name = params.find("name");
        if (!name || !name->isString()) {
            respondError(id, kErrInvalidParams,
                         "tools/call 缺少 name 字符串参数");
            return;
        }
        json::Value emptyArgs;
        emptyArgs.kind = json::Value::Kind::Object;
        const json::Value* args = params.find("arguments");
        const json::Value& a =
            (args && args->isObject()) ? *args : emptyArgs;

        CallOutcome r;
        const std::string& n = name->str;
        if (n == "scourge_check")
            r = toolCheck(a);
        else if (n == "scourge_interface")
            r = toolInterface(a);
        else if (n == "scourge_run")
            r = toolRun(a);
        else if (n == "scourge_repl_eval")
            r = toolReplEval(a);
        else {
            respondError(id, kErrInvalidParams, "未知工具 '" + n + "'");
            return;
        }

        if (!r.ok) {
            respondError(id, r.errCode, r.errMsg);
            return;
        }
        respondResult(id, [&](JsonWriter& w) {
            w.beginObject();
            w.key("content");
            w.beginArray();
            for (const auto& t : r.result.texts) {
                w.beginObject();
                w.pairString("type", "text");
                w.pairString("text", t);
                w.endObject();
            }
            w.endArray();
            w.pairBool("isError", r.result.isError);
            w.endObject();
        });
    }

    // ---- 工具实现（参数违规 → -32602；执行失败 → isError 结果） -------------

    CallOutcome paramError(const char* tool, const char* what) {
        CallOutcome r;
        r.ok = false;
        r.errCode = kErrInvalidParams;
        r.errMsg = std::string(tool) + "：" + what;
        return r;
    }

    CallOutcome fileNotFound(const std::string& path) {
        CallOutcome r;
        r.result.isError = true;
        r.result.texts.push_back("文件不存在: " + path);
        return r;
    }

    CallOutcome toolCheck(const json::Value& args) {
        const json::Value* files = args.find("files");
        if (!files || !files->isArray() || files->items.empty())
            return paramError("scourge_check", "需要非空字符串数组参数 files");
        for (const auto& f : files->items)
            if (!f.isString())
                return paramError("scourge_check", "files 的元素必须是字符串");

        unsigned jobs = 0;
        if (const json::Value* j = args.find("jobs");
            j && j->isInt() && j->integer > 0)
            jobs = static_cast<unsigned>(j->integer);

        CallOutcome r;
        bool anyError = false;
        if (files->items.size() == 1) {
            // 单文件：诊断 JSON 数组（与 CLI check --json 同构，代理消费最省上下文）。
            Session session;
            session.compile(fs::path(files->items[0].str), jobs);
            JsonWriter w(/*pretty=*/true);
            session.diag().emitJson(w);
            r.result.texts.push_back(w.take());
            anyError = session.diag().hasErrors();
        } else {
            // 多文件：{ "<file>": [诊断...] } 映射（每文件独立会话，路径即键）。
            JsonWriter w(/*pretty=*/true);
            w.beginObject();
            for (const auto& f : files->items) {
                Session session;
                session.compile(fs::path(f.str), jobs);
                w.key(f.str);
                session.diag().emitJson(w);
                anyError |= session.diag().hasErrors();
            }
            w.endObject();
            r.result.texts.push_back(w.take());
        }
        r.result.isError = anyError;
        return r;
    }

    CallOutcome toolInterface(const json::Value& args) {
        const json::Value* file = argStr(args, "file");
        if (!file)
            return paramError("scourge_interface", "需要字符串参数 file");
        fs::path p(file->str);
        if (!fs::exists(p)) return fileNotFound(file->str);

        CallOutcome r;
        Session session;
        session.compile(p);
        if (session.diag().hasErrors()) {
            JsonWriter w(/*pretty=*/true);
            session.diag().emitJson(w);
            r.result.isError = true;
            r.result.texts.push_back(
                "模块未通过全量验证（发布门禁），.sgi 生成中止：\n" + w.take());
            return r;
        }
        std::string sgi = session.emitInterface(p.stem().string());
        if (sgi.empty()) {
            r.result.isError = true;
            r.result.texts.push_back("interface 生成失败：模块未加载");
            return r;
        }
        r.result.texts.push_back(std::move(sgi)); // 仅返回内容，不写盘
        return r;
    }

    CallOutcome toolRun(const json::Value& args) {
        const json::Value* file = argStr(args, "file");
        if (!file) return paramError("scourge_run", "需要字符串参数 file");
        fs::path p(file->str);
        if (!fs::exists(p)) return fileNotFound(file->str);

        bool useCache = !argBool(args, "no_cache", false);
        CallOutcome r;
        RunOutcome run = runOnce(p, useCache, /*captureOut=*/true);
        r.result.texts.push_back(run.output); // content[0]：程序 print 输出
        if (!run.log.empty())
            r.result.texts.push_back(run.log); // content[1]：诊断 / panic
        r.result.isError = run.exitCode != 0;
        return r;
    }

    CallOutcome toolReplEval(const json::Value& args) {
        const json::Value* code = argStr(args, "code");
        if (!code) return paramError("scourge_repl_eval", "需要字符串参数 code");

        if (argBool(args, "reset", false)) repl.reset();
        ensureRepl();

        replOut.str("");
        replOut.clear();
        replErr.str("");
        replErr.clear();

        // 逐行喂入（与交互式 REPL 完全同规则：行尾冒号开块、空行收块）。
        // 末尾补一空行：未闭合块自动收束提交；空闲态空行被 REPL 忽略。
        const std::string& src = code->str;
        size_t pos = 0;
        while (pos < src.size() && !repl->finished()) {
            size_t nl = src.find('\n', pos);
            if (nl == std::string::npos) {
                repl->feed(src.substr(pos));
                break;
            }
            repl->feed(src.substr(pos, nl - pos));
            pos = nl + 1;
        }
        if (!repl->finished()) repl->feed("");
        if (repl->finished()) repl.reset(); // :quit 兜底：重建空会话保持可用

        CallOutcome r;
        r.result.texts.push_back(replOut.str());
        std::string errText = replErr.str();
        r.result.isError = !errText.empty(); // err 通道非空 = 诊断/panic/回滚
        if (!errText.empty()) r.result.texts.push_back(std::move(errText));
        return r;
    }

    void ensureRepl() {
        if (!repl) {
            replOut.str("");
            replOut.clear();
            replErr.str("");
            replErr.clear();
            repl = std::make_unique<Repl>(replOut, replErr);
        }
    }

    // ---- initialize / tools/list 报文 ----------------------------------------

    static void writeInitialize(JsonWriter& w) {
        w.beginObject();
        w.pairString("protocolVersion", kProtocolVersion);
        w.key("capabilities");
        w.beginObject();
        w.key("tools");
        w.beginObject();
        w.endObject();
        w.endObject();
        w.key("serverInfo");
        w.beginObject();
        w.pairString("name", "scourge");
        w.pairString("version", kScourgeVersion);
        w.endObject();
        w.endObject();
    }

    static void writeToolList(JsonWriter& w) {
        w.beginObject();
        w.key("tools");
        w.beginArray();
        writeTool(
            w, "scourge_check",
            "对 .sg 源文件执行全量验证（解析→类型→形状→契约），"
            "返回 JSON 诊断数组（与 scourge check --json 同构）。"
            "0 诊断为空数组；isError=true 表示存在错误级诊断。",
            [](JsonWriter& w2) {
                w2.key("files");
                w2.beginObject();
                w2.pairString("type", "array");
                w2.key("items");
                w2.beginObject();
                w2.pairString("type", "string");
                w2.endObject();
                w2.pairString("description", "入口 .sg 源文件路径列表");
                w2.endObject();
                writeIntProp(w2, "jobs", "并行度（省略 = 自动）");
            },
            {"files"});
        writeTool(
            w, "scourge_interface",
            "验证通过后生成 .sgi 接口契约文本（不写文件）。"
            "验证失败时返回 JSON 诊断（发布门禁）。",
            [](JsonWriter& w2) {
                writeStringProp(w2, "file", "入口 .sg 源文件路径");
            },
            {"file"});
        writeTool(
            w, "scourge_run",
            "编译为 SG-BC 字节码并执行，返回 print 输出与退出状态"
            "（.sgbc 指纹缓存增量加速；与 scourge run 零语义分叉；"
            "支持张量执行：字面量/逐元素/矩阵乘 @/多维下标）。",
            [](JsonWriter& w2) {
                writeStringProp(w2, "file", "入口 .sg 源文件路径");
                writeBoolProp(w2, "no_cache", "true = 跳过 .sgbc 缓存全量重编");
            },
            {"file"});
        writeTool(
            w, "scourge_repl_eval",
            "在持久 REPL 会话中求值：表达式回显求值、语句/块并入会话状态、"
            "import 并入模块图（试编译验收，失败回滚）。"
            "会话跨 tools/call 持久；多行输入与交互式 REPL 同规则"
            "（行尾冒号开块、空行收块，调用末尾自动补空行收束）。",
            [](JsonWriter& w2) {
                writeStringProp(
                    w2, "code",
                    "求值代码（可多行；支持表达式/语句/import/:check）");
                writeBoolProp(w2, "reset", "true = 先清空会话再求值");
            },
            {"code"});
        w.endArray();
        w.endObject();
    }
};

// ----------------------------------------------------------------------------
// 生命周期与 CLI 入口
// ----------------------------------------------------------------------------

McpServer::McpServer(std::istream& in, std::ostream& out)
    : state_(std::make_unique<State>(in, out)) {}

McpServer::~McpServer() = default;

int McpServer::run() { return state_->run(); }

int cmdMcp() {
    McpServer server(std::cin, std::cout);
    return server.run();
}

} // namespace scourge
