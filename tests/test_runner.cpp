// ============================================================================
//  Scourge 测试运行器 — P0 验证核心回归集
// ----------------------------------------------------------------------------
//  三类用例：
//    * 正例：合法程序 → 期望 0 错误 0 警告；
//    * 负例：非法程序 → 期望精确的诊断码多重集合（排序后比对，
//      保证"报什么错"与"报几次"都稳定——错误码即契约）；
//    * .sgi 黄金用例：generateSgi 输出逐字节比对（确定性要求，见设计文档第五章）。
//
//  管线与 main.cpp 的 Session 一致：Lexer → Parser → TypeChecker（单模块），
//  但不经过文件系统与模块 DAG——DAG 层错误（E5001/E5002）由 driver 职责测试覆盖。
// ============================================================================
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "scourge/BC/Compiler.hpp"
#include "scourge/BC/Serialize.hpp"
#include "scourge/BC/VM.hpp"
#include "scourge/Common/Capability.hpp"
#include "scourge/Common/Json.hpp"
#include "scourge/Interface/SgiGen.hpp"
#include "scourge/Sema/TypeChecker.hpp"
#include "scourge/Syntax/Lexer.hpp"
#include "scourge/Syntax/Parser.hpp"
#include "scourge/Tool/McpServer.hpp"
#include "scourge/Tool/Repl.hpp"
#include "scourge/Tool/TestRunner.hpp"

using namespace scourge;

namespace {

int g_passed = 0;
int g_failed = 0;

// ----------------------------------------------------------------------------
// 单模块测试会话：拥有 SourceMgr 与 DiagEngine（与 main.cpp Session 同构）。
// ----------------------------------------------------------------------------
class TestSession {
public:
    TestSession(std::string moduleName, std::string text)
        : src_(std::make_shared<SourceMgr>()) {
        FileId file = src_->addFile("<test>", std::move(text));
        diag_ = std::make_unique<DiagEngine>(*src_);
        Lexer lexer(*src_, file, *diag_);
        Parser parser(*src_, file, *diag_, lexer.tokenize());
        mod_ = parser.parseModule(std::move(moduleName));
        if (mod_) {
            ModuleRegistry empty; // 单模块：无跨模块依赖
            TypeChecker checker(*src_, *diag_, empty);
            syms_ = checker.check(*mod_);
        }
    }

    std::vector<Errc> codes() const {
        std::vector<Errc> v;
        for (const auto& d : diag_->all()) v.push_back(d.code);
        std::sort(v.begin(), v.end(), [](Errc a, Errc b) {
            return static_cast<uint16_t>(a) < static_cast<uint16_t>(b);
        });
        return v;
    }

    std::string sgi() const {
        return generateSgi(*src_, *mod_, syms_);
    }

private:
    std::shared_ptr<SourceMgr> src_;
    std::unique_ptr<DiagEngine> diag_;
    std::unique_ptr<Module> mod_;
    ModuleSymbols syms_;
};

std::string codesToString(const std::vector<Errc>& codes) {
    if (codes.empty()) return "(无)";
    std::string s;
    for (const auto& c : codes) {
        if (!s.empty()) s += " ";
        s += errcCode(c);
    }
    return s;
}

void expectCodes(const char* name, const std::vector<Errc>& got,
                 const std::vector<Errc>& want) {
    if (got == want) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n  期望: " << codesToString(want)
              << "\n  实际: " << codesToString(got) << "\n";
}

void expectSgi(const char* name, const std::string& got,
               const std::string& want) {
    if (got == want) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n--- 期望 ---\n"
              << want << "--- 实际 ---\n" << got << "---\n";
}

// ----------------------------------------------------------------------------
// VM 冒烟：全量检查 → SG-BC 编译 → 栈式执行，捕获 print 输出与退出码。
// 管线与 main.cpp cmdRun 同构（单模块、空 registry）。
// ----------------------------------------------------------------------------
struct RunResult {
    bool compiled = false;
    bool panicked = false;
    int exitCode = 0;
    std::string out;
    std::string panicText; ///< "panic: <msg> at <func>"
};

RunResult runProgram(const std::string& text,
                     uint64_t capOverride = ~uint64_t(0)) {
    RunResult r;
    auto src = std::make_shared<SourceMgr>();
    FileId file = src->addFile("<test>", text);
    DiagEngine diag(*src);
    Lexer lexer(*src, file, diag);
    Parser parser(*src, file, diag, lexer.tokenize());
    auto mod = parser.parseModule("test");
    if (!mod) return r;
    ModuleRegistry empty; // 单模块：无跨模块依赖
    TypeChecker checker(*src, diag, empty);
    checker.check(*mod);
    if (diag.hasErrors()) return r;

    bc::Compiler compiler(*src, diag, empty);
    r.compiled = compiler.compileModule(*mod);
    if (!r.compiled) return r;

    bc::VMOptions opts;
    opts.out = [&](const std::string& s) { r.out += s; };
    // 能力授予集（0.9.7）：单模块测试会话即模块自身声明；capOverride 供
    // 纵深防御用例强制改写（模拟绕过检查器的执行路径）。
    bc::Program prog = compiler.take();
    prog.capMask =
        (capOverride == ~uint64_t(0)) ? capMaskOf(*mod) : capOverride;
    bc::VM vm(std::move(prog), opts);
    if (!vm.run({"test"}, "test")) {
        r.panicked = true;
        const bc::Panic& p = vm.lastPanic();
        r.panicText = "panic: " + p.message + " at " + p.func;
        r.exitCode = 1;
    } else {
        r.exitCode = vm.exitCode();
    }
    return r;
}

void expectRun(const char* name, const RunResult& r, int wantExit,
               const std::string& wantOut) {
    if (r.compiled && !r.panicked && r.exitCode == wantExit &&
        r.out == wantOut) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n  compiled=" << r.compiled
              << " panicked=" << r.panicked << " exit=" << r.exitCode
              << (r.panicText.empty() ? "" : " (" + r.panicText + ")")
              << "\n  期望输出: [" << wantOut << "]\n  实际输出: [" << r.out
              << "]\n";
}

void expectPanic(const char* name, const RunResult& r,
                 const std::string& wantMsg) {
    if (r.compiled && r.panicked && r.exitCode == 1 &&
        r.panicText.find(wantMsg) != std::string::npos) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n  compiled=" << r.compiled
              << " panicked=" << r.panicked << " exit=" << r.exitCode
              << "\n  期望含: [" << wantMsg << "]\n  实际: [" << r.panicText
              << "]\n";
}

// ----------------------------------------------------------------------------
// REPL 会话：逐行喂入，捕获 out / err 双通道（管线与 cmdRepl 完全一致）。
// ----------------------------------------------------------------------------
struct ReplResult {
    std::string out;
    std::string err;
};

ReplResult runRepl(const std::vector<std::string>& lines) {
    ReplResult r;
    std::ostringstream out, err;
    Repl repl(out, err);
    for (const auto& l : lines) {
        if (!repl.feed(l)) break;
    }
    r.out = out.str();
    r.err = err.str();
    return r;
}

/// 期望 out 含 wantIn 且 err 为空（成功路径：无诊断噪声）。
void expectReplOut(const char* name, const ReplResult& r,
                   const std::string& wantIn) {
    if (r.out.find(wantIn) != std::string::npos && r.err.empty()) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n  期望 out 含: [" << wantIn
              << "]\n  实际 out: [" << r.out << "]\n  实际 err: [" << r.err
              << "]\n";
}

/// 期望 err 含 wantIn（wantIn 为空串 = 仅要求 err 非空）。
void expectReplErr(const char* name, const ReplResult& r,
                   const std::string& wantIn) {
    bool ok = !r.err.empty() &&
              (wantIn.empty() || r.err.find(wantIn) != std::string::npos);
    if (ok) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << "\n  期望 err 含: [" << wantIn
              << "]\n  实际 err: [" << r.err << "]\n  实际 out: [" << r.out
              << "]\n";
}

// ----------------------------------------------------------------------------
// SG-BC 编译辅助：源文本 → Program（序列化测试用，管线与 runProgram 同构）。
// ----------------------------------------------------------------------------
bool compileProgram(const std::string& text, bc::Program& out) {
    auto src = std::make_shared<SourceMgr>();
    FileId file = src->addFile("<test>", text);
    DiagEngine diag(*src);
    Lexer lexer(*src, file, diag);
    Parser parser(*src, file, diag, lexer.tokenize());
    auto mod = parser.parseModule("test");
    if (!mod) return false;
    ModuleRegistry empty;
    TypeChecker checker(*src, diag, empty);
    checker.check(*mod);
    if (diag.hasErrors()) return false;
    bc::Compiler compiler(*src, diag, empty);
    if (!compiler.compileModule(*mod)) return false;
    out = compiler.take();
    return true;
}

// ----------------------------------------------------------------------------
// MCP 服务器冒烟：全部请求一次性灌入 → 服务器循环 → 响应行序列（每行一报文）。
// McpServer 以 istream/ostream 注入驱动——与 CLI stdio 通路同一实现。
// ----------------------------------------------------------------------------
std::vector<std::string> runMcp(const std::vector<std::string>& requests) {
    std::string input;
    for (const auto& r : requests) {
        input += r;
        input += '\n';
    }
    std::istringstream in(input);
    std::ostringstream out;
    McpServer server(in, out);
    server.run();
    std::vector<std::string> lines;
    std::istringstream res(out.str());
    std::string line;
    while (std::getline(res, line))
        if (!line.empty()) lines.push_back(line);
    return lines;
}

/// 通用布尔断言（协议冒烟用）。
void expectTrue(const char* name, bool ok, const std::string& ctx = "") {
    if (ok) {
        ++g_passed;
        std::cout << "[通过] " << name << "\n";
        return;
    }
    ++g_failed;
    std::cout << "[失败] " << name << (ctx.empty() ? "" : "\n  " + ctx) << "\n";
}

} // namespace

// ============================================================================
// 用例集
// ============================================================================
int main() {
    // ---- 正例：合法程序，期望零诊断 ----------------------------------------
    {
        TestSession t("test", R"(def add(a: int, b: int) -> int:
    return a + b

def greet(name: str) -> str:
    return "hi, " + name

def main() -> int:
    x: int = add(1, 2)
    print(str(x))
    return x
)");
        expectCodes("正例/基础函数与调用", t.codes(), {});
    }
    {
        TestSession t("test", R"(def mat(a: Tensor[float, (2, 2)], b: Tensor[float, (2, 2)]) -> Tensor[float, (2, 2)]:
    c: Tensor[float, (2, 2)] = a @ b
    d: Tensor[float, (2, 2)] = c * 2.0
    e: float = d[0, 0]
    return d
)");
        expectCodes("正例/张量 matmul、标量广播与全索引", t.codes(), {});
    }
    {
        TestSession t("test", R"(def loss(pred: Tensor[float, (4, 1)], y: Tensor[float, (4, 1)]) -> float:
    e = pred - y
    return sum(e * e) / 4.0
)");
        expectCodes("正例/sum 归约返回元素类型", t.codes(), {});
    }
    {
        TestSession t("test", R"(def g() -> Result[int, str]:
    return err("bad")

def h() -> int:
    r: Result[int, str] = g()
    return 1
)");
        expectCodes("正例/Result 绑定到变量即视为消费", t.codes(), {});
    }

    // ---- 负例：类型与形状（E2xxx）------------------------------------------
    {
        TestSession t("test", "x: int = \"s\"\n");
        expectCodes("负例/E2001 赋值类型不匹配", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        TestSession t("test", "x = sum(\"abc\")\n");
        expectCodes("负例/E2001 sum 仅支持张量", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        TestSession t("test", "x = sqrt(\"a\")\n");
        expectCodes("负例/E2001 sqrt 仅支持数值", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        TestSession t("test", "x = max(\"abc\")\n");
        expectCodes("负例/E2001 max 仅支持张量归约", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        TestSession t("test", "x = transpose([1.0, 2.0])\n");
        expectCodes("负例/E2004 transpose 要求二维张量", t.codes(),
                    {Errc::E2004_ShapeMismatch});
    }
    {
        TestSession t("test",
                      "x = reshape([[1.0, 2.0], [3.0, 4.0]], 3, 3)\n");
        expectCodes("负例/E2004 reshape 元素总数不一致", t.codes(),
                    {Errc::E2004_ShapeMismatch});
    }
    {
        TestSession t("test", "x = []\n");
        expectCodes("负例/E2002 空列表无法推断", t.codes(),
                    {Errc::E2002_CannotInfer});
    }
    {
        TestSession t("test", "y = undefined_name\n");
        expectCodes("负例/E2003 未声明标识符", t.codes(),
                    {Errc::E2003_UnknownName});
    }
    {
        TestSession t("test", R"(def f(a: Tensor[int, (2, 3)], b: Tensor[int, (3, 2)]) -> Tensor[int, (2, 3)]:
    return a + b
)");
        expectCodes("负例/E2004 张量形状不兼容", t.codes(),
                    {Errc::E2004_ShapeMismatch});
    }
    {
        TestSession t("test", R"(def g() -> Result[int, str]:
    return err("bad")

def h() -> int:
    g()
    return 1
)");
        expectCodes("负例/E2005 Result 被丢弃", t.codes(),
                    {Errc::E2005_ResultNotConsumed});
    }
    {
        TestSession t("test", R"(def f(c: bool) -> int:
    if c:
        return 1
    return "s"
)");
        expectCodes("负例/E2006 返回类型不一致", t.codes(),
                    {Errc::E2006_ReturnTypeConflict});
    }
    {
        TestSession t("test", R"(def f() -> int:
    return 1

def f() -> int:
    return 2
)");
        expectCodes("负例/E2007 重复定义", t.codes(),
                    {Errc::E2007_DuplicateDefinition});
    }
    {
        TestSession t("test", "x: Foo = 1\n");
        expectCodes("负例/E2008 未知类型名", t.codes(),
                    {Errc::E2008_UnknownType});
    }
    {
        TestSession t("test", "def f() -> None:\n    break\n");
        expectCodes("负例/E2009 break 不在循环内", t.codes(),
                    {Errc::E2009_BreakOutsideLoop});
    }

    // ---- 负例：契约（E3xxx / W3001）----------------------------------------
    {
        TestSession t("test", R"(def pos(x: int) -> int:
    """
    requires: x > 0
    """
    return x

def main() -> int:
    return pos(-5)
)");
        expectCodes("负例/E3001 调用点静态违反 requires", t.codes(),
                    {Errc::E3001_ContractViolated});
    }
    {
        TestSession t("test", R"(def f(x: int) -> int:
    """
    ensures: result == x + 1
    """
    return x + 1
)");
        expectCodes("负例/W3001 ensures 降级为运行时断言", t.codes(),
                    {Errc::W3001_ContractDowngraded});
    }

    // ---- 负例：语法（E1xxx）--------------------------------------------------
    {
        TestSession t("test", "def f()\n");
        expectCodes("负例/E1005 def 缺少冒号", t.codes(),
                    {Errc::E1005_UnexpectedToken});
    }
    {
        // 0.9.3：Dict 字面量随注解/自推断放行（A 节类型系统），
        // 显式拒绝收窄为非标量键（E2001）。
        TestSession t("test", "x = {[1]: 2}\n");
        expectCodes("负例/E2001 字典键必须是标量", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        TestSession t("test", "x = 1 < 2 < 3\n");
        expectCodes("负例/E1007 链式比较被拒绝", t.codes(),
                    {Errc::E1007_UnsupportedFeature});
    }
    {
        TestSession t("test", "s = \"abc\n");
        expectCodes("负例/E1002 字符串未终止", t.codes(),
                    {Errc::E1002_UnterminatedString});
    }
    {
        TestSession t("test", "def f() -> int:\n\treturn 1\n");
        expectCodes("负例/E1004 Tab 缩进被拒绝", t.codes(),
                    {Errc::E1004_BadIndentation});
    }

    // ---- .sgi 黄金用例 -------------------------------------------------------
    {
        TestSession t("golden", R"(import math_utils

@pure
def add(a: int, b: int) -> int:
    """两数相加。
    requires: a >= 0
    ensures: result == a + b
    """
    return a + b

def helper(x: int) -> int:
    return x
)");
        expectSgi("黄金/.sgi 确定性输出（@pure 标记 + 契约规范化 + 描述过滤）",
                  t.sgi(),
                  "# scourge interface v1（scourge interface 工具生成，请勿手改）\n"
                  "# 模块: golden\n"
                  "# 本文件是接口契约：依赖方只看这里，无需阅读实现。\n"
                  "\n"
                  "module golden\n"
                  "\n"
                  "import math_utils\n"
                  "\n"
                  "@pure\n"
                  "def add(a: int, b: int) -> int:\n"
                  "    \"\"\"两数相加。\"\"\"\n"
                  "    requires: a >= 0\n"
                  "    ensures: result == a + b\n"
                  "\n"
                  "def helper(x: int) -> int:\n");
    }

    // ---- 正例：A 阶段前端补全（try/except / async-await / @extern）---------
    {
        TestSession t("test", R"(def f(x: int) -> int:
    try:
        return x // (x - x)
    except:
        return 0
)");
        expectCodes("正例/try-except panic 恢复路径", t.codes(), {});
    }
    {
        TestSession t("test", R"(async def task(x: int) -> int:
    return x + 1

async def main() -> int:
    v: int = await task(1)
    return v
)");
        expectCodes("正例/async def 与 await", t.codes(), {});
    }
    {
        TestSession t("test", R"(capability ffi

@extern("c")
def c_sin(x: float) -> float:
    pass
)");
        expectCodes("正例/@extern C ABI 纯声明", t.codes(), {});
    }

    // ---- 负例：await 作用域与 extern 声明（E2010 / E1007）-------------------
    {
        TestSession t("test", R"(def f() -> int:
    return await g()

def g() -> int:
    return 1
)");
        expectCodes("负例/E2010 await 在非 async 函数", t.codes(),
                    {Errc::E2010_AwaitOutsideAsync});
    }
    {
        TestSession t("test", R"(capability ffi

@extern("c")
def c_sin(x: float) -> float:
    return x
)");
        expectCodes("负例/E1007 extern 函数体必须是 pass", t.codes(),
                    {Errc::E1007_UnsupportedFeature});
    }
    {
        TestSession t("test", R"(def f(x: int) -> int:
    try:
        return x
    except ValueError:
        return 0
)");
        expectCodes("负例/E1007 类型化 except 被拒绝", t.codes(),
                    {Errc::E1007_UnsupportedFeature});
    }
    {
        TestSession t("test", R"(def f(x: int) -> int:
    try:
        return x
    except:
        return 0
    except:
        return 1
)");
        expectCodes("负例/多条 except 子句被拒绝", t.codes(),
                    {Errc::E1007_UnsupportedFeature});
    }
    {
        TestSession t("test", R"(def f(x: int) -> int:
    try:
        return x
    return 0
)");
        expectCodes("负例/E1005 try 缺少 except", t.codes(),
                    {Errc::E1005_UnexpectedToken});
    }

    // ---- O 节 FFI 宽类型域：变参 @extern 与 data_ptr（Sema 层契约）----------
    {
        TestSession t("test", R"(capability ffi

@extern("msvcrt.dll")
def printf(fmt: str, ...) -> int:
    pass

def main() -> int:
    printf("x=%d", 1)
    return 0
)");
        expectCodes("正例/变参 @extern 声明与调用", t.codes(), {});
    }
    {
        TestSession t("test", "def f(a: int, ...) -> int:\n    pass\n");
        expectCodes("负例/E1007 变参标记仅限 @extern", t.codes(),
                    {Errc::E1007_UnsupportedFeature});
    }
    {
        TestSession t("test", R"(capability ffi

def main() -> int:
    lst: List[int] = [1, 2, 3]
    p = data_ptr(lst)
    return 0
)");
        expectCodes("负例/E2001 data_ptr 拒绝 List", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        // 下限：变参调用实参数 < 命名参数个数
        TestSession t("test", R"(capability ffi

@extern("msvcrt.dll")
def printf(fmt: str, ...) -> int:
    pass

def main() -> int:
    printf()
    return 0
)");
        expectCodes("负例/E2001 变参调用实参数低于下限", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }
    {
        // 上限：20 与 VM 变参桩栈槽容量（16+4）对齐，21 实参编译期拦截
        std::string src =
            "capability ffi\n\n@extern(\"msvcrt.dll\")\n"
            "def printf(fmt: str, ...) -> int:\n    pass\n\n"
            "def main() -> int:\n    printf(\"x\", 1";
        for (int i = 2; i <= 21; ++i) src += ", " + std::to_string(i);
        src += ")\n    return 0\n";
        TestSession t("test", src);
        expectCodes("负例/E2001 变参调用实参数超上限 20", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }

    // ---- 诊断上限：错误数达到 kMaxErrors 后停止收集并给出 fatal 提示 ---------
    {
        std::string src;
        for (int i = 0; i < 150; ++i)
            src += "y" + std::to_string(i) + " = undefined" + std::to_string(i) + "\n";
        TestSession t("test", src);
        std::vector<Errc> want;
        want.push_back(Errc::E1005_UnexpectedToken); // fatal 上限提示
        for (int i = 0; i < 99; ++i) want.push_back(Errc::E2003_UnknownName);
        expectCodes("负例/错误上限 100 停止收集", t.codes(), want);
    }

    // ---- .sgi 黄金用例（A 阶段扩展：@extern / async 输出）-------------------
    {
        TestSession t("golden", R"(capability ffi

@extern("c")
def c_sin(x: float) -> float:
    pass

async def task(x: int) -> int:
    return x + 1
)");
        expectSgi("黄金/.sgi 输出 @extern 与 async 标记", t.sgi(),
                  "# scourge interface v1（scourge interface 工具生成，请勿手改）\n"
                  "# 模块: golden\n"
                  "# 本文件是接口契约：依赖方只看这里，无需阅读实现。\n"
                  "\n"
                  "module golden\n"
                  "\n"
                  "@extern(\"c\")\n"
                  "def c_sin(x: float) -> float:\n"
                  "\n"
                  "async def task(x: int) -> int:\n");
    }

    // ---- VM 冒烟（SG-BC 编译 + 栈式执行；输出回调捕获 print）--------------
    {
        RunResult r = runProgram(R"(def main() -> int:
    print(7 // 2, 7 % 3, 2 ** 10)
    print(7 / 2)
    print("ab" * 2 + "!")
    print(True, False, None)
    return 0
)");
        expectRun("正例/VM 算术与字符串", r, 0,
                  "3 1 1024\n3.5\nabab!\nTrue False None\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    total: int = 0
    for i in range(5):
        total += i
    n: int = 0
    acc: int = 0
    while True:
        n += 1
        if n > 10:
            break
        if n % 2 == 0:
            continue
        acc += n
    print(total, acc)
    return 0
)");
        expectRun("正例/VM 循环与分支", r, 0, "10 25\n");
    }
    {
        RunResult r = runProgram(R"(def div(a: int, b: int) -> Result[int, str]:
    if b == 0:
        return err("除数为零")
    return ok(a // b)

def main() -> int:
    r: Result[int, str] = div(10, 2)
    print(r.is_ok(), r.unwrap(0))
    e: Result[int, str] = div(1, 0)
    print(e.is_ok(), e.unwrap_or(-1))
    return 0
)");
        expectRun("正例/VM 错误即值 Result", r, 0, "True 5\nFalse -1\n");
    }
    // ---- 能力模型（0.9.7）：静态 E5004 门禁 + 运行时纵深防御 --------------
    {
        // 静态门禁：无声明使用 io 内建 → E5004（域级一条，定位首个使用点）
        TestSession s("capneg", "def main() -> int:\n    s = read_text(\"x\")\n    return 0\n");
        expectCodes("负例/能力未声明 E5004", s.codes(),
                    {Errc::E5004_CapabilityNotGranted});
    }
    {
        // 声明域不足：io 已授权但 net 内建 → E5004
        TestSession s(
            "capneg2",
            "capability io\n\ndef main() -> int:\n    h = net_connect(\"h\", 1)\n    return 0\n");
        expectCodes("负例/声明域不足 E5004", s.codes(),
                    {Errc::E5004_CapabilityNotGranted});
    }
    {
        // 授权正例：capability io → write/read 正常执行
        RunResult r = runProgram(R"(capability io

def main() -> int:
    p = "cap_smoke_tmp.txt"
    print(write_text(p, "ok"))
    print(read_text(p))
    return 0
)");
        expectRun("正例/VM 能力授权 io", r, 0, "2\nok\n");
    }
    {
        // 纵深防御：手搓 capMask=0 模拟绕过检查器的执行路径（旧缓存/外部
        // 构造 SG-BC）→ 运行时门禁独立拦截，消息与 AOT sg_cap_check 逐字一致
        RunResult r = runProgram(R"(capability io

def main() -> int:
    print(write_text("cap_zw_tmp.txt", "ok"))
    return 0
)",
                                 0);
        expectPanic("纵深/VM 运行时门禁拦截",
                    r, "capability 'io' not granted (write_text)");
    }
    {
        // O 节：data_ptr 地址导出（str intern 池 / 张量原生缓冲），仅断言非零
        RunResult r = runProgram(R"(capability ffi

def main() -> int:
    s = "hello"
    p = data_ptr(s)
    t = [[1.0, 2.0], [3.0, 4.0]]
    q = data_ptr(t)
    print(p > 0, q > 0)
    return 0
)");
        expectRun("正例/VM data_ptr 地址导出非零", r, 0, "True True\n");
    }
    {
        RunResult r = runProgram(R"(def boom(a: int, b: int) -> int:
    return a // b

def main() -> int:
    x: int = 0
    try:
        x = boom(1, 0)
    except:
        x = -2
    print(x)
    return 0
)");
        expectRun("正例/VM try-except 捕获 panic", r, 0, "-2\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    print("before")
    print(1 // 0)
    print("unreachable")
    return 0
)");
        expectPanic("负例/VM 未捕获 panic 整除零", r,
                    "panic: 整数整除零 at test.main");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    xs: List[str] = ["a", "b"]
    print(xs[5])
    return 0
)");
        expectPanic("负例/VM 索引越界 panic", r, "列表索引越界：5（长度 2）");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    return 42
)");
        expectRun("正例/VM main 返回值即退出码", r, 42, "");
    }
    // ---- 张量执行（P1：字面量/逐元素/矩阵乘/多维下标进入字节码）------------
    {
        RunResult r = runProgram(R"(def main() -> int:
    print([[1.0, 2.0], [3.0, 4.0]])
    return 0
)");
        expectRun("张量/print 嵌套括号格式", r, 0,
                  "Tensor(2, 2) [[1.0, 2.0], [3.0, 4.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a = [[1.0, 2.0], [3.0, 4.0]]
    print(a * 2.0 + 1.0)
    print(a - [[0.5, 0.5], [0.5, 0.5]])
    print(a / 2)
    return 0
)");
        expectRun("张量/逐元素与标量广播", r, 0,
                  "Tensor(2, 2) [[3.0, 5.0], [7.0, 9.0]]\n"
                  "Tensor(2, 2) [[0.5, 1.5], [2.5, 3.5]]\n"
                  "Tensor(2, 2) [[0.5, 1.0], [1.5, 2.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    print([[1.0, 2.0], [3.0, 4.0]] @ [[5.0, 6.0], [7.0, 8.0]])
    return 0
)");
        expectRun("张量/矩阵乘 @", r, 0,
                  "Tensor(2, 2) [[19.0, 22.0], [43.0, 50.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    print(sum([[1, 2, 3], [4, 5, 6]]))
    print(sum([[1.5, 2.5]]))
    print(sum([[1.0, 2.0]] * 3) / 2)
    return 0
)");
        expectRun("张量/sum 归约（int/float/表达式）", r, 0, "21\n4.0\n4.5\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    print(median([[1.0, 2.0], [3.0, 4.0]]))
    print(median([5.0, 1.0, 3.0]))
    print(median([1, 2, 3, 4]))
    print(median([3, 1, 2]))
    return 0
)");
        expectRun("统计/median 排序取中（张量/列表/奇偶）", r, 0,
                  "2.5\n3.0\n2.5\n2.0\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    try:
        print(median([sqrt(-1.0), 1.0]))
    except:
        print("neg-nan")
    return 0
)");
        expectRun("统计/median NaN 显式拒绝", r, 0, "neg-nan\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    xs: List[int] = [3, 1, 2]
    xs.append(4)
    fs: List[float] = []
    fs.append(1.5)
    bs: List[bool] = []
    bs.append(True)
    ss: List[str] = ["a"]
    ss.append("b")
    alias: List[int] = xs
    alias.append(0)
    print(xs)
    print(fs)
    print(bs)
    print(ss)
    return 0
)");
        expectRun("列表/append 标量全域与别名共享（空列表首插定型）", r, 0,
                  "[3, 1, 2, 4, 0]\n[1.5]\n[True]\n[\"a\", \"b\"]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a: List[int] = [x * x for x in range(4)]
    b: List[int] = [x for x in a if x % 2 == 0]
    c: List[str] = [ch for ch in "ab"]
    print(a)
    print(b)
    print(c)
    print(len(a))
    return 0
)");
        expectRun("列表/推导式（range/List/str 源 + 过滤 + len）", r, 0,
                  "[0, 1, 4, 9]\n[0, 4]\n[\"a\", \"b\"]\n4\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    for ch in "abc":
        print(ch)
    acc = ""
    for c in "hello":
        acc = c + acc
    print(acc)
    for x in "":
        print("never")
    return 0
)");
        expectRun("字符串/for 字节遍历（含反转拼接与空串零循环）", r, 0,
                  "a\nb\nc\nolleh\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    s = "abc"
    print(s[0])
    print(s[-1])
    try:
        print(s[5])
    except:
        print("caught")
    return 0
)");
        expectRun("字符串/下标（正/负归一/越界 panic 可捕获）", r, 0,
                  "a\nc\ncaught\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    d: Dict[str, int] = {"a": 1, "b": 2}
    print(d["a"])
    d["c"] = 3
    print(d["c"])
    print(len(d))
    print("b" in d)
    for k in d:
        print(k)
    print(d)
    return 0
)");
        expectRun("字典/字面量 str 键 len in for 遍历 print（插入序）", r, 0,
                  "1\n3\n3\nTrue\na\nb\nc\n{\"a\": 1, \"b\": 2, \"c\": 3}\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    d: Dict[str, int] = {"a": 1}
    try:
        print(d["zz"])
    except:
        print("caught")
    return 0
)");
        expectRun("字典/缺键 panic 可捕获", r, 0, "caught\n");
    }
    {
        RunResult r = runProgram(
            "capability time\n\ndef main() -> int:\n    t0 = now()\n    print(now() >= t0)\n"
            "    print(now() > 0.0)\n    return 0\n");
        expectRun("内建/now 单调时钟", r, 0, "True\nTrue\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a = 1.5
    b = 2.5
    row: Tensor[float, (?,)] = [a, b]
    print(row)
    print(sum([a, b, 3]))
    print([[a, 2.0], [3.0, 4.0]] @ [[1.0], [1.0]])
    return 0
)");
        expectRun("张量/运行时构造 NewTensor（一维/嵌套/@）", r, 0,
                  "Tensor(2) [1.5, 2.5]\n"
                  "7.0\n"
                  "Tensor(2, 1) [[3.5], [7.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    x = 0.5
    t: Tensor[float, (2, 2)] = [[x, x], [x, x]]
    print(t[1, 1])
    return 0
)");
        expectRun("张量/运行时构造 + 注解形状校验", r, 0, "0.5\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    print(abs(-3))
    print(abs(-2.5))
    print(sqrt(9))
    print(sqrt(2.25))
    print(exp(0.0))
    print(log(1.0))
    print(abs([[-1.0, 2.0], [-3.0, 4.0]]))
    print(sqrt([[1.0, 4.0], [9.0, 16.0]]))
    return 0
)");
        expectRun("张量/数学内建 abs/sqrt/exp/log", r, 0,
                  "3\n2.5\n3.0\n1.5\n1.0\n0.0\n"
                  "Tensor(2, 2) [[1.0, 2.0], [3.0, 4.0]]\n"
                  "Tensor(2, 2) [[1.0, 2.0], [3.0, 4.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a = [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
    print(transpose(a))
    print(reshape(a, 3, 2))
    print(reshape(a, 6))
    print(max(a))
    print(min(a))
    print(max([3, 1, 4, 1, 5]))
    return 0
)");
        expectRun("张量/transpose reshape max min", r, 0,
                  "Tensor(3, 2) [[1.0, 4.0], [2.0, 5.0], [3.0, 6.0]]\n"
                  "Tensor(3, 2) [[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]]\n"
                  "Tensor(6) [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]\n"
                  "6.0\n1.0\n5\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    t = [[1.0, 2.0, 3.0], [4.0, 5.0, 6.0]]
    print(t[1, 2])
    print(t[0])
    print(t[-1, 0])
    return 0
)");
        expectRun("张量/下标读取（标量/子张量/负下标）", r, 0,
                  "6.0\n"
                  "Tensor(3) [1.0, 2.0, 3.0]\n"
                  "4.0\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    t = [[1, 2], [3, 4]]
    t[0, 1] = 9
    print(t)
    print(t[1, 0] + 1)
    return 0
)");
        expectRun("张量/整型张量下标赋值", r, 0,
                  "Tensor(2, 2) [[1, 9], [3, 4]]\n"
                  "4\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a = [[1.0, 2.0], [3.0, 4.0]]
    a = a * 2.0
    print(a)
    return 0
)");
        expectRun("张量/重绑定逐元素", r, 0,
                  "Tensor(2, 2) [[2.0, 4.0], [6.0, 8.0]]\n");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    a = [[1.0, 2.0]]
    b = [[1.0], [2.0]]
    print(a @ b)
    return 0
)");
        expectRun("张量/非方阵 matmul 形状", r, 0,
                  "Tensor(1, 1) [[5.0]]\n");
    }
    {
        // 动态维（?）在编译期通过统一，运行时形状不符 → panic
        RunResult r = runProgram(R"(def main() -> int:
    x: Tensor[float, (?, 2)] = [[1.0, 2.0]]
    y: Tensor[float, (?, 2)] = [[1.0, 2.0], [3.0, 4.0]]
    print(x + y)
    return 0
)");
        expectPanic("张量/运行时形状不兼容 panic", r, "张量形状不兼容");
    }
    {
        // 动态维 matmul 运行时内维不符 → panic
        RunResult r = runProgram(R"(def main() -> int:
    a: Tensor[float, (?, 3)] = [[1.0, 2.0, 3.0]]
    b: Tensor[float, (?, 2)] = [[1.0, 2.0], [3.0, 4.0]]
    print(a @ b)
    return 0
)");
        expectPanic("张量/运行时 matmul 维度不匹配 panic", r,
                    "matmul 维度不匹配");
    }
    {
        RunResult r = runProgram(R"(def main() -> int:
    t = [[1.0, 2.0], [3.0, 4.0]]
    print(t[0, 5])
    return 0
)");
        expectPanic("张量/下标越界 panic", r, "张量索引越界");
    }
    {
        // 部分索引赋值：检查器要求全秩索引（显式拒绝，E2004）
        TestSession t("test", R"(def f(m: Tensor[float, (2, 2)]) -> int:
    m[0] = 1.0
    return 0
)");
        expectCodes("张量/部分索引赋值检查期拒绝 E2004", t.codes(),
                    {Errc::E2004_ShapeMismatch});
    }
    {
        // str() 与跨函数传参：张量作为函数值传递并参与运算
        RunResult r = runProgram(R"(def scale(m: Tensor[float, (2, 2)], k: float) -> Tensor[float, (2, 2)]:
    return m * k

def main() -> int:
    s = str(scale([[1.0, 2.0], [3.0, 4.0]], 3.0))
    print(s)
    return 0
)");
        expectRun("张量/str() 与跨函数传参", r, 0,
                  "Tensor(2, 2) [[3.0, 6.0], [9.0, 12.0]]\n");
    }
    {
        // 序列化：张量常量经 encode/decode 往返后执行输出与原件一致
        bc::Program p1;
        const bool ok = compileProgram(R"(def main() -> int:
    print([[1.0, 2.0], [3.0, 4.0]] @ [[2.0], [1.0]])
    print([[1, 2], [3, 4]] * 2)
    return 0
)",
                                       p1);
        if (!ok) {
            ++g_failed;
            std::cout << "[失败] 序列化/张量样例编译失败\n";
        } else {
            bc::CacheMeta m1;
            m1.toolchain = "test-toolchain";
            m1.entry = "test";
            m1.modules = {"test"};
            m1.paths = {"tensor.sg"};
            m1.sourceHash = 42;
            const std::string b1 = bc::encodeProgram(p1, m1);
            bc::Program p2;
            bc::CacheMeta m2;
            if (!bc::decodeProgram(b1, p2, m2)) {
                ++g_failed;
                std::cout << "[失败] 序列化/张量往返解码失败\n";
            } else {
                std::string o1, o2;
                bc::VMOptions v1, v2;
                v1.out = [&](const std::string& s) { o1 += s; };
                v2.out = [&](const std::string& s) { o2 += s; };
                bc::VM(p1, v1).run({"test"}, "test");
                bc::VM(p2, v2).run({"test"}, "test");
                const bool want = o1 == "Tensor(2, 1) [[4.0], [10.0]]\n"
                                       "Tensor(2, 2) [[2, 4], [6, 8]]\n";
                if (want && o1 == o2) {
                    ++g_passed;
                    std::cout << "[通过] 序列化/张量常量往返语义保真\n";
                } else {
                    ++g_failed;
                    std::cout << "[失败] 序列化/张量常量往返语义保真 o1=[" << o1
                              << "] o2=[" << o2 << "]\n";
                }
            }
        }
    }
    {
        // REPL：张量表达式即时求值
        ReplResult rr = runRepl({"[[1.0, 2.0]] @ [[3.0], [4.0]]"});
        expectReplOut("张量/REPL 矩阵乘求值", rr, "Tensor(1, 1) [[11.0]]");
    }
    {
        // 检查器：张量整体比较仍显式拒绝（E2001，逐元素 + 归约显式表达）
        TestSession t("test", R"(def f(a: Tensor[float, (2, 2)]) -> bool:
    return a == a
)");
        expectCodes("张量/整体比较仍拒绝 E2001", t.codes(),
                    {Errc::E2001_TypeMismatch});
    }

    // ---- REPL 会话（P1-C：表达式即时求值 + 会话状态） ------------------------
    {
        ReplResult r = runRepl({"1 + 2"});
        expectReplOut("REPL/表达式即时求值", r, "3");
    }
    {
        // 赋值入会话 → 后续表达式复用全局
        ReplResult r = runRepl({"x = 5", "x * 2"});
        expectReplOut("REPL/赋值入会话并复用", r, "10");
    }
    {
        // 块定义（行尾冒号 + 空行结束）→ 跨轮调用
        ReplResult r = runRepl({
            "def sq(n: int) -> int:",
            "    return n * n",
            "",
            "sq(7)",
        });
        expectReplOut("REPL/块定义与调用", r, "49");
    }
    {
        // print 调用按语句执行，不二次包装（不产生 "None" 回显）
        ReplResult r = runRepl({"print(\"hi\")"});
        expectReplOut("REPL/print 调用不二次包装", r, "hi");
    }
    {
        // 错误恢复：表达式失败 → 诊断入 err，后续求值不受影响
        ReplResult r = runRepl({"1 +", "4 + 5"});
        expectReplErr("REPL/表达式错误后恢复/诊断", r, "E1005");
        if (r.out.find("9") != std::string::npos) {
            ++g_passed;
            std::cout << "[通过] REPL/表达式错误后恢复/求值\n";
        } else {
            ++g_failed;
            std::cout << "[失败] REPL/表达式错误后恢复/求值\n  实际 out: ["
                      << r.out << "]\n";
        }
    }
    {
        // 定义失败回滚：坏定义不入会话（诊断 + 回滚提示），先前状态仍可用
        ReplResult r = runRepl({
            "y = 1",
            "def bad() -> int:",
            "    return \"str\"",
            "",
            "y + 1",
        });
        expectReplErr("REPL/定义失败回滚/诊断", r, "已回滚");
        if (r.out.find("2") != std::string::npos) {
            ++g_passed;
            std::cout << "[通过] REPL/定义失败回滚/会话保持\n";
        } else {
            ++g_failed;
            std::cout << "[失败] REPL/定义失败回滚/会话保持\n  实际 out: ["
                      << r.out << "]\n";
        }
    }
    {
        // 注解赋值归入会话（词法分类覆盖 "name:" 形态）
        ReplResult r = runRepl({"n: int = 6", "n + 1"});
        expectReplOut("REPL/注解赋值入会话", r, "7");
    }
    {
        // :check 对当前会话状态执行全量验证
        ReplResult r = runRepl({"z = 3", ":check"});
        expectReplOut("REPL/:check 全量验证", r, "OK");
    }
    {
        // :reset 清空会话状态 → 旧名不可用（检查器报错）
        ReplResult r = runRepl({"w = 1", ":reset", "w"});
        expectReplErr("REPL/:reset 清空状态", r, "");
    }
    {
        // import 不存在模块 → E5001 诊断 + 回滚（模块图会话的显式拒绝路径）
        ReplResult r = runRepl({"import foo"});
        expectReplErr("REPL/import 未知模块回滚", r, "已回滚");
    }
    {
        // 未知命令提示
        ReplResult r = runRepl({":nope"});
        expectReplErr("REPL/未知命令", r, "未知命令");
    }
    {
        // 运行时 panic 不丢会话（下一轮求值仍可用）
        ReplResult r = runRepl({"1 // 0", "7 - 3"});
        expectReplErr("REPL/panic 后会话保持", r, "panic");
        if (r.out.find("4") != std::string::npos) {
            ++g_passed;
            std::cout << "[通过] REPL/panic 后求值恢复\n";
        } else {
            ++g_failed;
            std::cout << "[失败] REPL/panic 后求值恢复\n  实际 out: ["
                      << r.out << "]\n";
        }
    }

    // ---- SG-BC 序列化（P1-D：.sgbc 缓存基础） ---------------------------------
    {
        // FNV-1a 64 已知向量：钉死实现，保证源指纹跨构建稳定
        const bool ok = bc::fnv1a64("") == 0xcbf29ce484222325ull &&
                        bc::fnv1a64("a") == 0xaf63dc4c8601ec8cull &&
                        bc::fnv1a64("foobar") == 0x85944171f73967e8ull;
        if (ok) {
            ++g_passed;
            std::cout << "[通过] 序列化/FNV-1a 已知向量\n";
        } else {
            ++g_failed;
            std::cout << "[失败] 序列化/FNV-1a 已知向量\n";
        }
    }
    {
        bc::Program p1;
        const bool ok = compileProgram(R"(def add(a: int, b: int) -> int:
    return a + b

def main() -> int:
    print(add(2, 3))
    return 0
)",
                                       p1);
        if (!ok) {
            ++g_failed;
            std::cout << "[失败] 序列化/样例编译失败\n";
        } else {
            bc::CacheMeta m1;
            m1.toolchain = "test-toolchain";
            m1.entry = "test";
            m1.modules = {"test"};
            m1.paths = {"hello.sg"};
            m1.sourceHash = 1234567890;
            const std::string b1 = bc::encodeProgram(p1, m1);

            bc::Program p2;
            bc::CacheMeta m2;
            if (!bc::decodeProgram(b1, p2, m2)) {
                ++g_failed;
                std::cout << "[失败] 序列化/往返解码失败\n";
            } else {
                const bool metaOk = m2.toolchain == m1.toolchain &&
                                    m2.entry == m1.entry &&
                                    m2.sourceHash == m1.sourceHash &&
                                    m2.modules == m1.modules &&
                                    m2.paths == m1.paths;
                const std::string b2 = bc::encodeProgram(p2, m2);
                if (b1 == b2 && metaOk) {
                    ++g_passed;
                    std::cout << "[通过] 序列化/往返字节等价\n";
                } else {
                    ++g_failed;
                    std::cout << "[失败] 序列化/往返字节等价 metaOk=" << metaOk
                              << " 字节差=" << (b1 != b2) << "\n";
                }

                // 语义保真：缓存重建的 Program 执行输出与原件一致
                bc::VMOptions vo;
                std::string out;
                vo.out = [&](const std::string& s) { out += s; };
                bc::VM vm(p2, vo);
                if (vm.run({"test"}, "test") && out == "5\n") {
                    ++g_passed;
                    std::cout << "[通过] 序列化/缓存程序语义保真\n";
                } else {
                    ++g_failed;
                    std::cout << "[失败] 序列化/缓存程序语义保真 out=[" << out
                              << "]\n";
                }
            }

            // 拒绝篡改：魔数错误 / 截断 / 尾随垃圾 → 一律解码失败
            std::string badMagic = b1;
            badMagic[0] = 'X';
            std::string truncated = b1.substr(0, b1.size() / 2);
            std::string tail = b1;
            tail.push_back('\0');
            bc::Program pt;
            bc::CacheMeta mt;
            const bool allRejected = !bc::decodeProgram(badMagic, pt, mt) &&
                                     !bc::decodeProgram(truncated, pt, mt) &&
                                     !bc::decodeProgram(tail, pt, mt);
            if (allRejected) {
                ++g_passed;
                std::cout << "[通过] 序列化/篡改拒绝\n";
            } else {
                ++g_failed;
                std::cout << "[失败] 序列化/篡改拒绝\n";
            }
        }
    }
    {
        // O 节：FuncBC 变参位持久化（v8 kFlagVariadic）——encode/decode 往返
        // 保真。缺失时缓存命中 isVariadic 丢为 false，float 变参退回定参 ABI
        // 读错 GP 影子槽（verify_o_wide 缓存命中分支实锤的回归钉子）。
        bc::Program p1;
        const bool ok = compileProgram(
            "capability ffi\n\n@extern(\"msvcrt.dll\")\n"
            "def printf(fmt: str, ...) -> int:\n    pass\n",
            p1);
        if (!ok) {
            ++g_failed;
            std::cout << "[失败] 序列化/变参样例编译失败\n";
        } else {
            bc::CacheMeta m1;
            m1.toolchain = "test-toolchain";
            m1.entry = "test";
            m1.modules = {"test"};
            m1.paths = {"variadic.sg"};
            m1.sourceHash = 7;
            const std::string b1 = bc::encodeProgram(p1, m1);
            bc::Program p2;
            bc::CacheMeta m2;
            if (!bc::decodeProgram(b1, p2, m2)) {
                ++g_failed;
                std::cout << "[失败] 序列化/变参往返解码失败\n";
            } else {
                bool variadicKept = false;
                for (const auto& f : p2.funcs)
                    if (f.name == "printf") variadicKept = f.isVariadic;
                if (variadicKept) {
                    ++g_passed;
                    std::cout << "[通过] 序列化/变参位往返保真\n";
                } else {
                    ++g_failed;
                    std::cout << "[失败] 序列化/变参位往返保真\n";
                }
            }
        }
    }

    // ---- REPL import 会话（P1-E：会话 = 活动模块图） -------------------------
    {
        namespace fs = std::filesystem;
        const fs::path oldCwd = fs::current_path();
        const fs::path tmp =
            fs::temp_directory_path() / "scourge_repl_import_test";
        fs::create_directories(tmp);
        {
            std::ofstream(tmp / "mlib.sg")
                << "def twice(x: int) -> int:\n    return x * 2\n";
            std::ofstream(tmp / "mapp.sg")
                << "import mlib\n\ndef quad(x: int) -> int:\n"
                   "    return mlib.twice(mlib.twice(x))\n";
            std::ofstream(tmp / "badmod.sg")
                << "def broken() -> int:\n    return \"str\"\n";
        }
        fs::current_path(tmp);

        {
            // import 验收 + 依赖模块图跨模块调用（mapp → mlib）
            ReplResult r = runRepl({"import mapp", "mapp.quad(3)"});
            expectReplOut("REPL/import 跨模块调用", r, "12");
        }
        {
            // from-import 显式拒绝（REPL 会话只收整模块 import）
            ReplResult r = runRepl({"from mlib import twice"});
            expectReplErr("REPL/from-import 显式拒绝", r, "from-import");
        }
        {
            // import 缺失模块 → E5001 诊断 + 回滚，会话保持可用
            ReplResult r = runRepl({"import ghost", "2 + 3"});
            expectReplErr("REPL/import 模块缺失回滚", r, "已回滚");
            if (r.out.find("5") != std::string::npos) {
                ++g_passed;
                std::cout << "[通过] REPL/import 模块缺失会话保持\n";
            } else {
                ++g_failed;
                std::cout << "[失败] REPL/import 模块缺失会话保持 out=["
                          << r.out << "]\n";
            }
        }
        {
            // import 含类型错误的模块 → 检查诊断 + 回滚（E2002）
            ReplResult r = runRepl({"import badmod"});
            expectReplErr("REPL/import 坏模块拒绝", r, "已回滚");
        }
        {
            // 活动模块图：每轮从磁盘重新加载，模块编辑即时生效
            std::ostringstream out, err;
            Repl repl(out, err);
            repl.feed("import mapp");
            repl.feed("mapp.quad(3)"); // 2*2*3 = 12
            std::ofstream(tmp / "mlib.sg", std::ios::trunc)
                << "def twice(x: int) -> int:\n    return x * 3\n";
            repl.feed("mapp.quad(3)"); // 3*3*3 = 27（twice=x*3，磁盘编辑即时生效）
            const std::string o = out.str();
            if (o.find("12") != std::string::npos &&
                o.find("27") != std::string::npos) {
                ++g_passed;
                std::cout << "[通过] REPL/活动模块图磁盘编辑即时生效\n";
            } else {
                ++g_failed;
                std::cout << "[失败] REPL/活动模块图磁盘编辑即时生效 out=["
                          << o << "] err=[" << err.str() << "]\n";
            }
        }
        {
            // :reset 连同 import 模块图一起清空 → 旧模块名不可见（E2003）
            ReplResult r = runRepl({"import mapp", ":reset", "mapp.quad(3)"});
            expectReplErr("REPL/:reset 清空模块图", r, "E2003");
        }

        fs::current_path(oldCwd);
        fs::remove_all(tmp);
    }

    // ---- JSON 解析器（MCP 请求端对称件：JsonWriter 为生成端） ----------------
    {
        json::Value v;
        std::string err;
        bool ok = json::parse(R"({"a":1,"b":[true,null,"x\n中"],"c":{"d":-2.5}})",
                              v, err);
        expectTrue("JSON/嵌套结构解析", ok && v.isObject());
        if (ok) {
            const json::Value* a = v.find("a");
            const json::Value* b = v.find("b");
            const json::Value* c = v.find("c");
            expectTrue("JSON/整数", a && a->isInt() && a->integer == 1);
            expectTrue("JSON/数组与转义",
                       b && b->isArray() && b->items.size() == 3 &&
                           b->items[0].isBool() && b->items[0].boolean &&
                           b->items[1].isNull() && b->items[2].isString() &&
                           b->items[2].str == "x\n中");
            const json::Value* d = c ? c->find("d") : nullptr;
            expectTrue("JSON/负浮点", d && d->isDouble() && d->number == -2.5);
        }
        json::Value bad;
        expectTrue("JSON/语法错误拒绝",
                   !json::parse("{bad", bad, err) && !err.empty());
        expectTrue("JSON/尾随内容拒绝",
                   !json::parse("[] x", bad, err) && !err.empty());
        expectTrue("JSON/嵌套过深拒绝",
                   !json::parse(std::string(70, '[') + std::string(70, ']'),
                                bad, err));
    }

    // ---- MCP 服务器（P2：stdio JSON-RPC 2.0，外部 AI 代理零胶水接入） ---------
    {
        namespace fs = std::filesystem;
        const fs::path oldCwd = fs::current_path();
        const fs::path tmp = fs::temp_directory_path() / "scourge_mcp_test";
        fs::create_directories(tmp);
        {
            std::ofstream(tmp / "mcpgood.sg")
                << "def main() -> int:\n    print(\"mcp-run-ok\")\n    return 0\n";
            std::ofstream(tmp / "mcpbad.sg")
                << "def f() -> int:\n    return zzz\n";
        }
        fs::current_path(tmp);

        // 协议层：initialize 握手（id 回显 / protocolVersion / serverInfo），
        // 通知（无 id）不产生响应。
        {
            auto lines = runMcp({
                R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}}})",
                R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
            });
            json::Value v;
            std::string err;
            bool ok = lines.size() == 1 && json::parse(lines[0], v, err);
            expectTrue("MCP/initialize 唯一响应（通知不响应）", ok,
                       "响应行数 " + std::to_string(lines.size()));
            if (ok) {
                const json::Value* res = v.find("result");
                const json::Value* id = v.find("id");
                const json::Value* pv =
                    res ? res->find("protocolVersion") : nullptr;
                const json::Value* si = res ? res->find("serverInfo") : nullptr;
                const json::Value* nm = si ? si->find("name") : nullptr;
                const json::Value* ver = si ? si->find("version") : nullptr;
                expectTrue("MCP/initialize id 回显",
                           id && id->isInt() && id->integer == 7);
                expectTrue("MCP/initialize 协议版本",
                           pv && pv->isString() && pv->str == "2024-11-05");
                expectTrue("MCP/initialize serverInfo",
                           nm && nm->str == "scourge" && ver && ver->isString() &&
                               !ver->str.empty());
            }
        }
        // 工具清单：四工具齐全且各带 inputSchema
        {
            auto lines = runMcp({R"({"jsonrpc":"2.0","id":"tl","method":"tools/list"})"});
            json::Value v;
            std::string err;
            bool ok = lines.size() == 1 && json::parse(lines[0], v, err);
            expectTrue("MCP/tools/list 响应", ok);
            if (ok) {
                const json::Value* res = v.find("result");
                const json::Value* tools =
                    res ? res->find("tools") : nullptr;
                bool namesOk = tools && tools->isArray() && tools->items.size() == 4;
                if (namesOk) {
                    const char* want[4] = {"scourge_check", "scourge_interface",
                                           "scourge_run", "scourge_repl_eval"};
                    for (const char* w : want) {
                        bool found = false;
                        for (const auto& t : tools->items) {
                            const json::Value* n = t.find("name");
                            const json::Value* s = t.find("inputSchema");
                            if (n && n->isString() && n->str == w && s &&
                                s->isObject() && s->find("properties"))
                                found = true;
                        }
                        if (!found) namesOk = false;
                    }
                }
                expectTrue("MCP/tools/list 四工具与 inputSchema", namesOk);
            }
        }
        // 协议层错误：未知方法 -32601 / 未知工具 -32602 / 解析错误 -32700
        {
            auto lines = runMcp({
                R"({"jsonrpc":"2.0","id":2,"method":"no/such"})",
                R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ghost","arguments":{}}})",
                "{bad json",
            });
            expectTrue("MCP/错误响应行数", lines.size() == 3);
            if (lines.size() == 3) {
                json::Value a, b, c;
                std::string err;
                bool pa = json::parse(lines[0], a, err);
                bool pb = json::parse(lines[1], b, err);
                bool pc = json::parse(lines[2], c, err);
                auto ecode = [](const json::Value& v) -> int64_t {
                    const json::Value* e = v.find("error");
                    const json::Value* code = e ? e->find("code") : nullptr;
                    return (code && code->isInt()) ? code->integer : -999999;
                };
                expectTrue("MCP/未知方法 -32601",
                           pa && ecode(a) == -32601 && a.find("id") &&
                               a.find("id")->isInt() &&
                               a.find("id")->integer == 2);
                expectTrue("MCP/未知工具 -32602", pb && ecode(b) == -32602);
                expectTrue("MCP/解析错误 -32700 且 id 为 null",
                           pc && ecode(c) == -32700 && c.find("id") &&
                               c.find("id")->isNull());
            }
        }
        // 工具层：check 正例（0 诊断）与负例（E2003 + isError）
        {
            auto lines = runMcp({
                R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"scourge_check","arguments":{"files":["mcpgood.sg"]}}})",
                R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"scourge_check","arguments":{"files":["mcpbad.sg"]}}})",
            });
            expectTrue("MCP/check 两响应", lines.size() == 2);
            if (lines.size() == 2) {
                json::Value good, bad;
                std::string err;
                bool pg = json::parse(lines[0], good, err);
                bool pb = json::parse(lines[1], bad, err);
                auto firstText = [](const json::Value& v) -> const json::Value* {
                    const json::Value* res = v.find("result");
                    const json::Value* content =
                        res ? res->find("content") : nullptr;
                    if (!content || !content->isArray() || content->items.empty())
                        return nullptr;
                    return content->items[0].find("text");
                };
                const json::Value* gt = pg ? firstText(good) : nullptr;
                expectTrue("MCP/check 正例 0 诊断",
                           gt && gt->isString() &&
                               gt->str.find("\"code\"") == std::string::npos);
                const json::Value* bt = pb ? firstText(bad) : nullptr;
                const json::Value* res2 = pb ? bad.find("result") : nullptr;
                expectTrue("MCP/check 负例 E2003 + isError",
                           bt && bt->isString() &&
                               bt->str.find("E2003") != std::string::npos &&
                               res2 && res2->find("isError") &&
                               res2->find("isError")->isBool() &&
                               res2->find("isError")->boolean);
            }
        }
        // 工具层：run（print 输出捕获；与 CLI run 共用 Runner 核心）
        {
            auto lines = runMcp({
                R"({"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"scourge_run","arguments":{"file":"mcpgood.sg"}}})",
            });
            expectTrue("MCP/run 响应", lines.size() == 1);
            if (lines.size() == 1) {
                json::Value v;
                std::string err;
                bool ok = json::parse(lines[0], v, err);
                const json::Value* res = ok ? v.find("result") : nullptr;
                const json::Value* content =
                    res ? res->find("content") : nullptr;
                bool textOk = false;
                if (content && content->isArray() && !content->items.empty()) {
                    const json::Value* t = content->items[0].find("text");
                    textOk = t && t->isString() &&
                             t->str.find("mcp-run-ok") != std::string::npos;
                }
                expectTrue("MCP/run print 输出捕获", textOk);
            }
        }
        // 工具层：repl_eval 会话跨 tools/call 持久 + 失败恢复 + reset
        {
            auto call = [](int id, const std::string& argsJson) {
                return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                       R"(,"method":"tools/call","params":{"name":"scourge_repl_eval","arguments":)" +
                       argsJson + "}}";
            };
            auto lines = runMcp({
                call(30, R"({"code":"1 + 2"})"),
                call(31, R"({"code":"acc = 10"})"),
                call(32, R"({"code":"acc + 5"})"),
                call(33, R"({"code":"1 +"})"),
                call(34, R"({"code":"acc * 2"})"),
                call(35, R"({"code":"acc + 1","reset":true})"),
            });
            expectTrue("MCP/repl_eval 六响应", lines.size() == 6);
            if (lines.size() == 6) {
                std::vector<json::Value> vs(6);
                std::string err;
                bool allOk = true;
                for (size_t i = 0; i < 6; ++i)
                    allOk = allOk && json::parse(lines[i], vs[i], err);
                expectTrue("MCP/repl_eval 响应解析", allOk);
                if (allOk) {
                    auto textOf = [](const json::Value& v) -> std::string {
                        const json::Value* res = v.find("result");
                        const json::Value* content =
                            res ? res->find("content") : nullptr;
                        if (!content || !content->isArray() ||
                            content->items.empty())
                            return "";
                        const json::Value* t = content->items[0].find("text");
                        return t && t->isString() ? t->str : "";
                    };
                    auto logOf = [](const json::Value& v) -> std::string {
                        const json::Value* res = v.find("result");
                        const json::Value* content =
                            res ? res->find("content") : nullptr;
                        if (!content || !content->isArray() ||
                            content->items.size() < 2)
                            return "";
                        const json::Value* t = content->items[1].find("text");
                        return t && t->isString() ? t->str : "";
                    };
                    auto isError = [](const json::Value& v) -> bool {
                        const json::Value* res = v.find("result");
                        const json::Value* e =
                            res ? res->find("isError") : nullptr;
                        return e && e->isBool() && e->boolean;
                    };
                    expectTrue("MCP/repl_eval 表达式求值",
                               textOf(vs[0]).find("3") != std::string::npos &&
                                   !isError(vs[0]));
                    expectTrue("MCP/repl_eval 赋值静默",
                               textOf(vs[1]).empty() && !isError(vs[1]));
                    expectTrue("MCP/repl_eval 会话持久",
                               textOf(vs[2]).find("15") != std::string::npos &&
                                   !isError(vs[2]));
                    expectTrue("MCP/repl_eval 失败 isError + E1005",
                               isError(vs[3]) &&
                                   logOf(vs[3]).find("E1005") !=
                                       std::string::npos);
                    expectTrue("MCP/repl_eval 失败后恢复",
                               textOf(vs[4]).find("20") != std::string::npos &&
                                   !isError(vs[4]));
                    expectTrue("MCP/repl_eval reset 后 E2003",
                               isError(vs[5]) &&
                                   logOf(vs[5]).find("E2003") !=
                                       std::string::npos);
                }
            }
        }

        fs::current_path(oldCwd);
        fs::remove_all(tmp);
    }

    // ---- 测试框架纯助手（scourge::sgt，设计第六章第 5 条）------------------
    {
        sgt::Rng a(42), b(42), c(43);
        bool same = true, diff = false;
        for (int i = 0; i < 8; ++i) {
            const uint64_t x = a.next(), y = b.next(), z = c.next();
            same &= (x == y);
            diff |= (x != z);
        }
        expectTrue("sgt/Rng 同种子流逐字节一致", same);
        expectTrue("sgt/Rng 异种子流分离", diff);
        expectTrue("sgt/Rng 字符串种子派生非零",
                   sgt::Rng("m/add").state != 0);
    }
    {
        // renderFloat：定点小数、去尾零但保留小数点后至少一位、无指数记法。
        expectTrue("sgt/renderFloat 0.5", sgt::renderFloat(0.5) == "0.5");
        expectTrue("sgt/renderFloat 100.0", sgt::renderFloat(100.0) == "100.0");
        expectTrue("sgt/renderFloat 0.001", sgt::renderFloat(0.001) == "0.001");
        expectTrue("sgt/renderFloat -0.5", sgt::renderFloat(-0.5) == "-0.5");
        expectTrue("sgt/renderFloat 2.5", sgt::renderFloat(2.5) == "2.5");
        expectTrue("sgt/renderFloat 极小值归零形态",
                   sgt::renderFloat(-0.0000001) == "-0.0");
    }
    {
        expectTrue("sgt/renderString 普通",
                   sgt::renderString("abc") == "\"abc\"");
        expectTrue("sgt/renderString 空串", sgt::renderString("") == "\"\"");
        expectTrue("sgt/renderString 转义引号",
                   sgt::renderString("a\"b") == "\"a\\\"b\"");
        expectTrue("sgt/renderString 转义反斜杠",
                   sgt::renderString("a\\b") == "\"a\\\\b\"");
        expectTrue("sgt/renderString 转义换行",
                   sgt::renderString("a\nb") == "\"a\\nb\"");
    }
    {
        std::map<std::string, bool> locals{{"f", true}, {"MAX", true}};
        expectTrue("sgt/qualify 本地名限定",
                   sgt::qualifyContractText("x == f(x) + MAX", "m", locals) ==
                       "x == m.f(x) + m.MAX");
        expectTrue("sgt/qualify result 改绑",
                   sgt::qualifyContractText("result >= 0", "m", locals) ==
                       "_sgt_r >= 0");
        expectTrue("sgt/qualify 成员访问不重复限定",
                   sgt::qualifyContractText("other.f(x)", "m", locals) ==
                       "other.f(x)");
        expectTrue("sgt/qualify 字符串字面量不动",
                   sgt::qualifyContractText("s == \"f MAX\"", "m", locals) ==
                       "s == \"f MAX\"");
        expectTrue("sgt/qualify 内建不限定",
                   sgt::qualifyContractText("len(x) > 0", "m", locals) ==
                       "len(x) > 0");
    }

    std::cout << "\n" << g_passed << " 通过, " << g_failed << " 失败\n";
    return g_failed == 0 ? 0 : 1;
}
