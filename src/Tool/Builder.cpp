// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 工具链 — build 命令实现：AOT 三阶段流水线驱动
// ----------------------------------------------------------------------------
//  Stage A（前端）复用 Session 加载 + 全量检查；Stage C 本文件：
//    generateMLIR → 写 .mlir → 进程内 MLIR→LLVM IR（AOT/InProcess，等价
//    原 mlir-opt --convert-to-llvm + mlir-translate 两步）→ clang 链接
//    内嵌 C 运行时。
//
//  C 运行时（kRuntimeC 及后续段，分块于 include/scourge/Tool/BuilderRT*.hpp）
//  与 MLIRGen 的 extern 声明逐一对应（34 张量/标量 +
//  21 Result/List + 3 try 蹦床），语义纪律
//  与 SG-BC VM（src/BC/VM.cpp）逐字对齐：
//    * 整数 add/sub/mul/幂/取负溢出、除零 → panic（消息与 VM 相同）；
//    * '/' 真除法恒 float；'//' Python 向下取整；'%' 与除数同号；
//    * 浮点 print/str 用最短往返表示（15→16→17 位逐级尝试），补 ".0"；
//    * Result 载荷按位编码 / List etag 0-3，方法语义与 VM CallMethod 一致；
//  * try/except 蹦床（sg_try_push/sg_try_pop + 生成代码直调 CRT _setjmp）
//      处理器栈与 VM catches_ 同构，sg_panic 单一钩子：栈顶有 handler 则
//      longjmp（与 VM recover 一致），否则打印 + exit(1)；
//    * panic 输出 "panic: <msg> at <fn> (<file>:<line>)" 到 stderr + exit 1，
//      与 Runner 的 VM panic 文本逐字节一致（构建产物与 scourge run 双模一致）。
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS // std::getenv（MSVC 弃用告警，此处无并发安全诉求）

#include "scourge/Tool/Builder.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

// C 运行时四段（按拼接序 = C 依赖序）：基础设施 → 张量 → 容器 → 排序与 IO
#include "scourge/Tool/BuilderRTCore.hpp"
#include "scourge/Tool/BuilderRTTensor.hpp"
#include "scourge/Tool/BuilderRTColl.hpp"
#include "scourge/Tool/BuilderRTIo.hpp"
// 专项片段（追加在各段之后，复用前置段的基础设施）
#include "scourge/Tool/BuilderRTStr.hpp" // 字符串运行时片段（R 专项）
#include "scourge/Tool/BuilderRTStd.hpp" // 标准库广度片段（统计/倒序/哈希；M 专项）
#include "scourge/Tool/BuilderRTJson.hpp" // JSON 运行时片段（M 专项，vendored jsmn）
#include "scourge/Tool/BuilderRTHttp.hpp" // HTTP 服务端运行时片段（0.9.8）
#include "scourge/Tool/BuilderRTThread.hpp" // 真并发运行时片段（M1，thread 域）
#include "scourge/Tool/BuilderRTGui.hpp"    // GUI 运行时片段（M1+M2，gui 域）
#include "scourge/Tool/BuilderRTNet.hpp" // 网络/睡眠运行时片段（F/G 专项）
#include "scourge/Tool/BuilderRTSys.hpp" // 系统编程运行时片段（H 专项）
// GPU 字符串（--gpu 时写入）：kernel 源 + 分派层两段
#include "scourge/Tool/BuilderRTGpuPtx.hpp"
#include "scourge/Tool/BuilderRTGpuOcl.hpp"
#include "scourge/Tool/BuilderRTGpu.hpp"
#include "scourge/Tool/BuilderRTGpuOps.hpp"
#include "scourge/AOT/InProcess.hpp"
#include "scourge/AOT/MLIRGen.hpp"
#include "scourge/BC/Serialize.hpp"
#include "scourge/Common/Capability.hpp"
#include "scourge/Common/Version.hpp"
#include "scourge/Tool/Session.hpp"

namespace fs = std::filesystem;

// OS 头（selfTag 取编译器自身路径用）。注意：BuilderRT*.hpp 的原始字符串
// 内嵌的是生成给用户程序的 C 运行时源码，其中的 #include 是字符串文本——
// 编译器自身的 OS 头只能放在这里。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace scourge {

namespace {

// ----------------------------------------------------------------------------
// 内嵌 C 运行时：字符串常量按功能域分块至 include/scourge/Tool/BuilderRT*.hpp
// （拼接顺序见 buildOnce 写入序列；此处仅 include，不再内嵌原文）。
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// CUDA 第 1 层：GPU kernel PTX 与分派运行时（--gpu 时追加写入 _rt.c）
// PTX/OpenCL kernel 与分派层源码见 include/scourge/Tool/BuilderRTGpu*.hpp
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
// 外部命令驱动
// ----------------------------------------------------------------------------

/// 执行外部命令并捕获合并输出（stdout+stderr → 捕获文件 → 读回 → 删除）。
int runCapture(const std::string& exe, const std::string& args,
               const fs::path& capPath, std::string& out) {
    std::error_code ec;
    fs::remove(capPath, ec);
#ifdef _WIN32
    // Windows 命令解释器对以引号开头且含特殊字符的命令会剥首尾引号——
    // 外层再包一对引号（官方文档的标准规避法），内层各路径引号原样保留。
    std::string cmdline = "\"\"" + exe + "\" " + args + " > \"" +
                          capPath.string() + "\" 2>&1\"";
#else
    // POSIX: /bin/sh 直跑（双引号语义一致，无需外层规避对）。
    std::string cmdline = "\"" + exe + "\" " + args + " > '" +
                          capPath.string() + "' 2>&1";
#endif
    int rc = std::system(cmdline.c_str());
    out.clear();
    std::ifstream in(capPath, std::ios::binary);
    if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
    }
    fs::remove(capPath, ec);
    return rc;
}

/// 内嵌文本 → C 字符串字面量体（换行 → "\n" 续段，转义反斜杠与引号；
/// 用于把 kernel PTX 原文嵌入生成的 _rt.c，逐行可读便于检视）。
std::string cStringLiteral(const char* raw) {
    std::string out;
    for (const char* p = raw; *p; ++p) {
        switch (*p) {
        case '\n': out += "\\n\"\n\""; break;
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        default: out += *p; break;
        }
    }
    return out;
}

// ----------------------------------------------------------------------------
// AOT obj 增量缓存（设计文档 5.2：MLIR→LLVM→object 按（模块, 目标, 优化档）
// 缓存，链接必付）。缓存是加速项而非正确性依赖——读写失败一律静默降级，
// 对齐 .sgbc 缓存纪律（src/Tool/Runner.cpp）。
// ----------------------------------------------------------------------------

/// 缓存键格式版本：键成分或 obj 语义变化时递增，旧缓存整体失效。
constexpr const char* kObjCacheVer = "scourge-aot-obj-v1";

/// fnv1a64 包装（重载歧义防护）：bc::fnv1a64 有 (string_view, seed) 与
/// (const void*, len, seed=默认) 两个重载——const char* 实参会精确匹配后者，
/// seed 被当作 len 造成野读崩溃。字符串键一律经此包装显式走 string_view。
inline uint64_t fnvKey(std::string_view s,
                       uint64_t seed = 0xcbf29ce484222325ull) {
    return bc::fnv1a64(s, seed);
}

/// 链接两对象 → 可执行（目标感知）：
///   * 本机/交叉 Windows：clang 驱动链接（-lws2_32 -lshell32）；
///   * 交叉 POSIX：ld.lld 直链 musl 静态（crt1/crti/crtn + -lc -lm
///     -lgcc -lpthread -ldl；clang 驱动在此路径会找 GCC 的 crtbeginT.o，
///     musl sysroot 不提供，故绕开）。
/// 需在 buildOnce 可见作用域内取 targetWin/xo/llvmBin——以 lambda 闭包
/// 形式在 buildOnce 内构造。
/// 工具链标签：clang 路径 + 大小 + 修改时间——工具链升级即缓存失效。
std::string clangTag(const fs::path& clang) {
    std::error_code ec;
    uintmax_t size = fs::file_size(clang, ec);
    uint64_t sizeV = ec ? 0 : static_cast<uint64_t>(size);
    ec.clear();
    auto t = fs::last_write_time(clang, ec);
    uint64_t mtime = ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
    return clang.string() + "|" + std::to_string(sizeV) + "|" +
           std::to_string(mtime);
}

/// 编译器自体指纹：自身可执行文件路径 + 大小 + 修改时间。作为 mainObjKey
/// 的指纹源（对齐 clangTag 模式）：任何 TU 重编 → 重链 → 指纹变化 → obj
/// 缓存全量失效，无需逐 TU 登记时间戳。路径取值失败时降级为空路径——
/// 大小/mtime 退化为 0，指纹仍稳定（仅失去失效灵敏度，不产生错误命中）。
std::string selfTag() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) path.assign(buf, n);
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) path.assign(buf, static_cast<size_t>(n));
#endif
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    uint64_t sizeV = ec ? 0 : static_cast<uint64_t>(size);
    ec.clear();
    auto t = fs::last_write_time(path, ec);
    uint64_t mtime = ec ? 0 : static_cast<uint64_t>(t.time_since_epoch().count());
    return path + "|" + std::to_string(sizeV) + "|" + std::to_string(mtime);
}

/// main.obj 缓存键（五重一致性，对齐 .sgbc 五重校验）：格式版本、
/// 编译器自体指纹、工具链标签、源指纹（依赖层序逐模块：名/路径/归一化
/// 源文本）、优化档+gpuMode、入口。任一不符 → 未命中，全量重编。
uint64_t mainObjKey(const std::string& tag, bool gpuMode,
                    const std::string& entry,
                    const std::vector<std::string>& order,
                    const Session& session,
                    const std::string& targetTag) {
    uint64_t h = fnvKey(kObjCacheVer);
    // 编译器自体指纹（自身 exe 大小+mtime，对齐 clangTag 模式）：任何 TU
    // 重编必然重链 scourge.exe，指纹随之变化，全部 obj 缓存自动失效。
    // 旧方案仅哈希本 TU 的 __DATE__ __TIME__——LangToLLVM/MLIRGen 单独
    // 重编而 Builder.cpp 未动时指纹不变，旧 obj 带着过期代码生成行为继续
    // 命中（风险台账 P2）。版本标签同入键：发布版与开发版二进制互不共享。
    h = fnvKey(selfTag(), h);
    h = fnvKey(kScourgeVersionLabel, h);
    h = fnvKey(tag, h);
    h = fnvKey(targetTag, h); // 交叉目标（空 = 本机原生；对象不可跨目标复用）
    h = fnvKey(gpuMode ? "gpu" : "cpu", h);
    h = fnvKey("-O2", h);
    h = fnvKey(entry, h);
    for (const auto& name : order) {
        h = fnvKey(name, h);
        h = fnvKey(session.loadedModulePath(name), h);
        h = fnvKey(session.loadedSource(name), h);
    }
    return h;
}

/// 能力运行时门禁片段（0.9.7）：域表按 kAllCaps 生成（单一事实源，禁止
/// 手工平行名单）+ sg_cap_check 定义。置于 kRuntimeC 之后（sg_panic 可用），
/// kSysRuntimeC 的 sg_extern_resolve 入口查位依赖其 SG_CAP_FFI_BIT 宏。
/// @sg_caps 由 LangToLLVM 注入 main.obj（external constant），此处 extern。
std::string capRuntimeC() {
    std::string s =
        "\n/* ---- 能力运行时门禁（0.9.7）----\n"
        " * @sg_caps：程序能力授予掩码（LangToLLVM 注入 main.obj 的\n"
        " * external constant）。未授权即 panic，消息与 VM capPanicMsg\n"
        " * （Common/Capability.hpp 单点生成）逐字一致。域表由编译器按\n"
        " * kAllCaps 生成。 */\n"
        "extern unsigned long long sg_caps;\n"
        "#define SG_CAP_FFI_BIT " + std::to_string(capBit(Cap::Ffi)) +
        "ULL\n"
        "static const struct { unsigned long long bit; const char* name; }\n"
        "    sg_cap_names[] = {\n";
    for (Cap c : kAllCaps)
        s += "        {" + std::to_string(capBit(c)) + "ULL, \"" +
             std::string(capName(c)) + "\"},\n";
    s += "    };\n"
         "void sg_cap_check(unsigned long long bit, const char* what,\n"
         "                  const char* fn, const char* file, int64_t line) {\n"
         "    if ((sg_caps & bit) != 0) return;\n"
         "    const char* dom = \"?\";\n"
         "    for (unsigned i = 0;\n"
         "         i < sizeof sg_cap_names / sizeof sg_cap_names[0]; ++i)\n"
         "        if (sg_cap_names[i].bit == bit) {\n"
         "            dom = sg_cap_names[i].name;\n"
         "            break;\n"
         "        }\n"
         "    char msg[128];\n"
         "    snprintf(msg, sizeof msg, \"capability '%s' not granted (%s)\",\n"
         "             dom, what);\n"
         "    sg_panic(fn, file, line, msg);\n"
         "}\n";
    return s;
}

/// rt.obj 缓存键（源无关）：格式版本、工具链标签、gpuMode、运行时源文本。
/// 各运行时段（BuilderRT*.hpp）内嵌于编译器二进制，文本哈希入键——开发期
/// 改动运行时源自动失效，无需手工递增版本号。
uint64_t rtObjKey(const std::string& tag, bool gpuMode,
                  const std::string& targetTag) {
    uint64_t h = fnvKey(kObjCacheVer);
    h = fnvKey(tag, h);
    h = fnvKey(targetTag, h); // 同 mainObjKey：对象不可跨目标复用
    h = fnvKey(gpuMode ? "gpu" : "cpu", h);
    h = fnvKey(kRuntimeC, h);
    h = fnvKey(kTensorRuntimeC, h);
    h = fnvKey(kCollRuntimeC, h);
    h = fnvKey(kIoRuntimeC, h);
    h = fnvKey(capRuntimeC(), h); // 能力门禁片段（域表随 kAllCaps 变化自动失效）
    h = fnvKey(kStrRuntimeC, h);
    h = fnvKey(kNetRuntimeC, h);
    h = fnvKey(kSysRuntimeC, h);
    h = fnvKey(kStdRuntimeC, h);
    h = fnvKey(kJsonRuntimeC, h);
    h = fnvKey(kHttpRuntimeC, h);
    h = fnvKey(kThreadRuntimeC, h);
    h = fnvKey(kGuiRuntimeC, h);
    if (gpuMode) {
        h = fnvKey(kGpuRuntimeC, h);
        h = fnvKey(kGpuOpsRuntimeC, h);
        h = fnvKey(kGpuPtxC, h);
        h = fnvKey(kGpuOpenCLC, h);
    }
    return h;
}

/// 缓存键文件（十进制单行文本）。读取不符/失败即未命中。
bool readObjKey(const fs::path& p, uint64_t expect) {
    std::ifstream in(p);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line)) return false;
    return line == std::to_string(expect);
}

/// 写键（失败静默：下次按未命中全量重编，不影响正确性）。
void writeObjKey(const fs::path& p, uint64_t v) {
    std::ofstream of(p, std::ios::binary);
    if (of) of << v << "\n";
}

} // namespace

// ----------------------------------------------------------------------------
// 构建主流程
// ----------------------------------------------------------------------------

BuildOutcome buildOnce(const fs::path& inPath, const fs::path& outPath,
                       bool gpuMode, bool useCache, const CrossOpts& xo) {
    BuildOutcome b;
    b.exe = outPath;
    const std::string entry = inPath.stem().string();

    // ---- 交叉目标归类（缓存键成分，最先确定） ----
    // 链接库与 crt 选择跟随目标平台（而非宿主）：交叉到 Linux 与本机
    // Linux 的产物形态一致（POSIX 交叉走 musl 静态 sysroot + ld.lld 直链）。
    bool crossPosix = false, targetWin;
    if (xo.target.empty()) {
#ifdef _WIN32
        targetWin = true;
#else
        targetWin = false;
#endif
    } else {
        targetWin = xo.target.find("windows") != std::string::npos ||
                    xo.target.find("mingw") != std::string::npos;
        crossPosix = !targetWin;
    }
    std::string crossFlags; // 附加到每个 clang 调用（compile 阶段）
    std::string targetTag;  // 缓存键成分（compile + link 参数合集）
    if (!xo.target.empty()) {
        crossFlags += " --target=" + xo.target;
        targetTag += "t=" + xo.target;
        if (!xo.sysroot.empty()) {
            crossFlags += " --sysroot=" + xo.sysroot;
            targetTag += "|s=" + xo.sysroot;
        }
        if (!xo.cpu.empty()) {
            crossFlags += " -mcpu=" + xo.cpu;
            targetTag += "|c=" + xo.cpu;
        }
        if (!xo.march.empty()) {
            crossFlags += " -march=" + xo.march;
            targetTag += "|m=" + xo.march;
        }
        if (crossPosix && xo.staticLink) targetTag += "|static";
        if (gpuMode && crossPosix) {
            b.log = "构建中止：交叉模式暂不支持 --gpu（GPU 片段依赖宿主 "
                    "OpenCL 头与宿主 GPU；交叉目标无 GPU 语义）\n";
            b.exitCode = 1;
            return b;
        }
        if (crossPosix) {
            // POSIX 交叉 = musl 静态链接（动态需目标侧 ld-musl 与完整
            // libc.so，v1 不覆盖；静态产物零依赖最贴合嵌入式分发）。
            if (!xo.staticLink) {
                b.log = "构建中止：交叉 POSIX 目标要求 --static（musl 静态 "
                        "sysroot；动态链接需目标侧 ld-musl，暂不覆盖）\n";
                b.exitCode = 1;
                return b;
            }
            if (xo.sysroot.empty()) {
                b.log = "构建中止：交叉 POSIX 目标需要 --sysroot（用 "
                        "toolchain/build_musl_sysroot.sh 产出 musl 静态 "
                        "libc + 头文件）\n";
                b.exitCode = 1;
                return b;
            }
            fs::path sl = fs::path(xo.sysroot) / "lib";
            std::error_code ec;
            for (const char* f : {"libc.a", "crt1.o", "crti.o", "crtn.o"}) {
                if (!fs::exists(sl / f, ec)) {
                    b.log = "构建中止：sysroot 缺少 lib/" + std::string(f) +
                            "（不完整的 musl sysroot？" + sl.string() + "）\n";
                    b.exitCode = 1;
                    return b;
                }
            }
        }
    }

    // ---- 工具链定位（SCOURGE_LLVM_BIN 可覆盖；缓存键成分，最先确定） ----
    const char* envBin = std::getenv("SCOURGE_LLVM_BIN");
    fs::path llvmBin =
        (envBin && *envBin)
            ? fs::path(envBin)
            : fs::path(
#ifdef _WIN32
                  "D:/LLVM/bin"
#else
                  "/usr/bin"
#endif
              );
    fs::path clang = llvmBin /
#ifdef _WIN32
        "clang.exe";
#else
        "clang";
#endif
    {
        std::error_code ec;
        if (!fs::exists(clang, ec)) {
            // PATH 回退（Linux 宿主常规途径：/usr/bin/clang 在 PATH）
            const char* pe = std::getenv("PATH");
            bool found = false;
            if (pe) {
                std::string p = pe, delim =
#ifdef _WIN32
                    ";";
#else
                    ":";
#endif
                size_t st = 0;
                while (st <= p.size()) {
                    size_t en = p.find(delim, st);
                    std::string dir =
                        p.substr(st, en == std::string::npos ? std::string::npos
                                                             : en - st);
                    fs::path cand = fs::path(dir) /
#ifdef _WIN32
                        "clang.exe";
#else
                        "clang";
#endif
                    if (fs::exists(cand, ec)) {
                        clang = cand;
                        found = true;
                        break;
                    }
                    if (en == std::string::npos) break;
                    st = en + 1;
                }
            }
            if (!found) {
                b.log = "构建中止：未找到 " + clang.string() +
                        "（可用环境变量 SCOURGE_LLVM_BIN 指定 LLVM bin "
                        "目录）\n";
                b.exitCode = 1;
                return b;
            }
        }
    }

    // ---- 阶段 0：加载（解析）。全量检查延后至缓存未命中——与 run 缓存同
    //      纪律：命中即跳过检查/MLIR/转换/编译（源未变则上次构建已过门禁），
    //      仅付链接开销。 ----
    Session session;
    if (!session.load(inPath)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    auto order = session.modulesInDependencyOrder(entry);
    // 能力授予掩码（0.9.7）：依赖闭包全部模块的 capability 声明并集——
    // 与 run 命令的 Program.capMask 同一语义（Capability.hpp 位布局），
    // 经 convertToLLVMInProcess → LangToLLVM 以 @sg_caps 注入 main.obj。
    uint64_t sessionCaps = 0;
    for (const auto& name : order) {
        const Module* m = session.loadedModule(name);
        if (m) sessionCaps |= capMaskOf(*m);
    }

    // ---- 中间产物路径（与可执行同目录，便于检视） ----
    fs::path dir = outPath.parent_path();
    if (dir.empty()) dir = ".";
    std::string stem = outPath.stem().string();
    fs::path mlirPath = dir / (stem + ".mlir");
    fs::path llPath = dir / (stem + ".ll");
    fs::path rtPath = dir / (stem + "_rt.c");
    fs::path capPath = dir / (stem + ".buildcap");

    // ---- AOT obj 缓存目录（<stem>.sgbuild/obj，设计第零/五章命名）。
    //      目录不可建（权限/占用）→ 静默降级为无缓存单步直编。 ----
    fs::path cacheDir = dir / (stem + ".sgbuild") / "obj";
    bool cacheOk = false;
    if (useCache) {
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        cacheOk = !ec;
    }
    const uint64_t keyMain = mainObjKey(clangTag(clang), gpuMode, entry,
                                        order, session, targetTag);
    const uint64_t keyRt = rtObjKey(clangTag(clang), gpuMode, targetTag);

    std::string toolOut;

    // 链接执行器（目标感知）：交叉 POSIX = ld.lld 直链 musl 静态（clang
    // 驱动在此路径会找 GCC 的 crtbeginT.o/-lgcc，musl sysroot 不提供）；
    // Windows 目标 = clang 驱动（-lws2_32 -lshell32）；本机 POSIX =
    // clang 驱动（-lpthread -ldl -lm）。
    auto runLink = [&](const std::string& o1, const std::string& o2) -> int {
        if (crossPosix) {
            fs::path sl = fs::path(xo.sysroot) / "lib";
            fs::path lld = llvmBin /
#ifdef _WIN32
                "lld.exe"; // lld 通用驱动：需 -flavor gnu 显式选 ELF 后端
#else
                "ld.lld";
#endif
            std::error_code ec;
            if (!fs::exists(lld, ec)) lld = fs::path("ld.lld"); // PATH 回退
            std::string args =
#ifdef _WIN32
                "-flavor gnu "
#endif
                "-static";
            args += " \"" + o1 + "\" \"" + o2 + "\" \"" +
                    (sl / "crt1.o").string() + "\" \"" +
                    (sl / "crti.o").string() + "\" \"" +
                    (sl / "crtn.o").string() + "\" -L\"" + sl.string() +
                    "\" -lc -lm -lgcc -lpthread -ldl -o \"" +
                    outPath.string() + "\"";
            return runCapture(lld.string(), args, capPath, toolOut);
        }
        return runCapture(clang.string(),
                          "\"" + o1 + "\" \"" + o2 + "\" -o \"" +
                              outPath.string() + "\"" +
                              (targetWin ? " -lws2_32 -lshell32 -luser32 -lgdi32"
                                         : " -lpthread -ldl -lm"),
                          capPath, toolOut);
    };

    // ---- 缓存命中判定：双 obj 在盘且双键全等才链接（显式拒绝，无静默回退） ----
    if (cacheOk) {
        fs::path objMain = cacheDir / "main.obj";
        fs::path objRt = cacheDir / "rt.obj";
        std::error_code ec;
        if (fs::exists(objMain, ec) && fs::exists(objRt, ec) &&
            readObjKey(cacheDir / "main.key", keyMain) &&
            readObjKey(cacheDir / "rt.key", keyRt)) {
            int rc = runLink(objMain.string(), objRt.string());
            if (rc == 0) {
                b.fromCache = true;
                return b;
            }
            // 缓存对象损坏（链接失败）→ 失效整目录，就地回退全量重建。
            b.log = "AOT 缓存命中但链接失败，已失效缓存并全量重建\n" + toolOut;
            fs::remove_all(cacheDir, ec);
            fs::create_directories(cacheDir, ec);
            cacheOk = !ec;
        }
    }

    // ---- 阶段 1：全量检查（与 check/run 同一门禁；命中路径不付此开销） ----
    session.check();
    if (session.diag().hasErrors()) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 2：AST → 文本 MLIR（不支持构造显式 E1007） ----
    std::string mlir;
    if (!aot::generateMLIR(session, entry, mlir)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }

    {
        std::ofstream f(mlirPath, std::ios::binary);
        f << mlir;
    }
    {
        std::ofstream f(rtPath, std::ios::binary);
        f << kRuntimeC << kTensorRuntimeC << kCollRuntimeC << kIoRuntimeC;
        /* 能力运行时门禁片段（0.9.7）：域表按 kAllCaps 生成 + sg_cap_check
         * 定义；置于 kRuntimeC 之后（sg_panic 可用），kSysRuntimeC 的
         * sg_extern_resolve 入口查位依赖其 SG_CAP_FFI_BIT 宏。 */
        f << capRuntimeC();
        /* str/List 成员方法片段（0.9.4）：置于各基础段之后——复用
         * kCollRuntimeC 的 SgList 与 kRuntimeC 的 sg_dup/sg_panic；
         * 缓存键已计入该片段文本 */
        f << kStrRuntimeC;
        /* 网络/睡眠片段（0.9.4 F/G）：同 Str 片段模式，复用 kRuntimeC 的
         * sg_panic 与 kIoRuntimeC 的 sg_io_msg；winsock 链接见各链接命令。 */
        f << kNetRuntimeC;
        /* 系统编程片段（0.9.4 H）：args/env/时间/进程/信号，复用
         * kCollRuntimeC 的 SgList、kRuntimeC 的 sg_panic 与 kIoRuntimeC 的
         * sg_io_msg；shellapi 链接见各链接命令（-lshell32）。 */
        f << kSysRuntimeC;
        /* 标准库广度片段（0.9.4 M）：统计/倒序/哈希，复用 kTensorRuntimeC 的
         * SgTensor/sg_tsync/sg_tsize、kCollRuntimeC 的 SgList、kRuntimeC 的
         * sg_panic 与 math.h 定义。 */
        f << kStdRuntimeC;
        /* JSON 片段（0.9.4 M）：vendored jsmn（MIT）+ SgJson 适配层，复用
         * kCollRuntimeC 的 SgList 与 kRuntimeC 的 sg_dup/sg_panic。 */
        f << kJsonRuntimeC;
        /* HTTP 服务端片段（0.9.8）：置于 JSON 之后——复用 kNetRuntimeC 的
         * SOCKET/recv/sg_net_sockof 与 kJsonRuntimeC 的 SgJson 构造族。 */
        f << kHttpRuntimeC;
        /* 真并发片段（M1，thread 域）：线程/锁/通道，复用 kRuntimeC 的
         * sg_panic/sg_dup 与 kIoRuntimeC 的 sg_io_msg；POSIX 链接 -lpthread
         * 由各链接命令提供，Windows 用 _beginthreadex（kernel32，无额外链接）。 */
        f << kThreadRuntimeC;
        /* GUI 片段（M1+M2，gui 域）：Win32 窗口/控件/事件，复用 kCollRuntimeC
         * 的 SgDict/sg_dict_new（事件 = Dict[str,str]）与 kRuntimeC 的
         * sg_panic/sg_dup；Windows 链接 -luser32 -lgdi32，POSIX 侧为桩。 */
        f << kGuiRuntimeC;
        if (gpuMode) {
            /* GPU 分派层：内嵌 kernel 源（Windows 分支内定义，供 JIT 加载）
             * + 后端抽象分派函数（覆盖集 op，失败回退 CPU） */
            f << "\n#ifdef _WIN32\n/* 内嵌 kernel PTX（llc sm_120 产物，"
                 "cuModuleLoadData JIT） */\nstatic const char sg_gpu_ptx[] =\n\""
              << cStringLiteral(kGpuPtxC) << "\";\n"
                 "/* 内嵌 OpenCL C kernel 源（clBuildProgram 运行时 JIT，"
                 "与 sg_kernels.ll 镜像） */\nstatic const char sg_gpu_oclc[] =\n\""
              << cStringLiteral(kGpuOpenCLC) << "\";\n#endif\n"
              << kGpuRuntimeC << kGpuOpsRuntimeC;
        }
    }

    // ---- 阶段 3（合并）：进程内 MLIR → LLVM dialect → LLVM IR ----
    // gpuMode：GPU 覆盖集 op 保持 lang 层直达 LangToLLVM（调用换 sg_gpu_*，
    // 内联降级跳过）；.mlir 落盘产物与 CPU 模式逐字节相同。
    if (!aot::convertToLLVMInProcess(mlir, llPath.string(), toolOut, gpuMode,
                                     sessionCaps)) {
        std::ofstream cap(capPath, std::ios::binary);
        cap << toolOut;
        b.log = "构建中止：MLIR → LLVM IR 进程内转换失败\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    // ---- 阶段 4+5：clang 分段编译 → 增量链接（缓存可用路径） ----
    // main.obj ← .ll（依赖源+工具链+优化档）；rt.obj ← _rt.c（源无关）。
    // obj 编译即写缓存（产物落缓存目录），链接自缓存目录取用。
    if (cacheOk) {
        fs::path objMain = cacheDir / "main.obj";
        fs::path objRt = cacheDir / "rt.obj";
        int rc = runCapture(
            clang.string(),
            "-c -O2 -Wno-override-module -D_CRT_SECURE_NO_WARNINGS" +
                crossFlags + " \"" + llPath.string() + "\" -o \"" +
                objMain.string() + "\"",
            capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译 " + llPath.string() +
                    " 失败（exit " + std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        rc = runCapture(clang.string(),
                        "-c -O2 -D_CRT_SECURE_NO_WARNINGS" + crossFlags +
                            " \"" + rtPath.string() + "\" -o \"" +
                            objRt.string() + "\"",
                        capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译运行时失败（exit " +
                    std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        // obj 落盘成功后写键（写失败静默——下次按未命中处理）。
        writeObjKey(cacheDir / "main.key", keyMain);
        writeObjKey(cacheDir / "rt.key", keyRt);

        rc = runLink(objMain.string(), objRt.string());
        if (rc != 0) {
            b.log = "构建中止：链接失败（exit " + std::to_string(rc) +
                    "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        return b; // exitCode 0
    }

    // ---- 交叉 POSIX：强制两步（ld.lld 直链；clang 驱动在此路径会找
    //      GCC 的 crtbeginT.o/-lgcc，musl sysroot 不提供）。 ----
    if (crossPosix) {
        fs::path objMain = dir / (stem + ".obj");
        fs::path objRt = dir / (stem + "_rt.obj");
        int rc = runCapture(
            clang.string(),
            "-c -O2 -Wno-override-module -D_CRT_SECURE_NO_WARNINGS" +
                crossFlags + " \"" + llPath.string() + "\" -o \"" +
                objMain.string() + "\"",
            capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译 " + llPath.string() +
                    " 失败（exit " + std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        rc = runCapture(clang.string(),
                        "-c -O2 -D_CRT_SECURE_NO_WARNINGS" + crossFlags +
                            " \"" + rtPath.string() + "\" -o \"" +
                            objRt.string() + "\"",
                        capPath, toolOut);
        if (rc != 0) {
            b.log = "构建中止：clang 编译运行时失败（exit " +
                    std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        rc = runLink(objMain.string(), objRt.string());
        if (rc != 0) {
            b.log = "构建中止：链接失败（exit " + std::to_string(rc) +
                    "）\n" + toolOut;
            b.exitCode = 1;
            return b;
        }
        return b; // exitCode 0
    }

    // ---- 静默降级路径：单步编译链接（原命令形，probe4 验证过） ----
    int rc = runCapture(
        clang.string(),
        "-O2 -Wno-override-module -D_CRT_SECURE_NO_WARNINGS" + crossFlags +
            " \"" + llPath.string() + "\" \"" + rtPath.string() +
            "\" -o \"" + outPath.string() + "\"" +
            (targetWin ? " -lws2_32 -lshell32 -luser32 -lgdi32"
                       : " -lpthread -ldl -lm"),
        capPath, toolOut);
    if (rc != 0) {
        b.log = "构建中止：clang 链接失败（exit " + std::to_string(rc) +
                "）\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    return b; // exitCode 0
}

} // namespace scourge
