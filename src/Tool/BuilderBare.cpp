// Scourge build 命令 — aarch64 freestanding / --board 通道
// Frontend 复用 Session/MLIR/InProcess，差异在链接层：crt 三件套模板展开，
// ld.lld -T 直链无 libc，objcopy 出 raw bin；白名单外符号→能力域处方诊断。
#include "scourge/Tool/Builder.hpp"

#include "scourge/AOT/InProcess.hpp"
#include "scourge/AOT/MLIRGen.hpp"
#include "scourge/Tool/BuilderBareA64.hpp"
#include "scourge/Tool/BuilderBareRt.hpp"
#include "scourge/Tool/BuilderBareRtFmt.hpp"
#include "scourge/Tool/BuilderBareRtStr.hpp"
#include "scourge/Tool/BuilderShared.hpp"
#include "scourge/Tool/Session.hpp"

#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace scourge {

// ----------------------------------------------------------------------------
// 板级预设（boards/ 数据文件落地前的内置种子；仅展开为 BareOpts 字段，
// Builder 其余部分只认字段不认预设名）。
// ----------------------------------------------------------------------------

std::string applyBarePreset(const std::string& name, BareOpts& bo,
                            std::string& cpu) {
    if (name == "qemu-virt-a64") {
        // QEMU virt：PL011 @ 0x09000000，RAM 0x40000000 起；GB0 内有
        // flash/GIC（device），GB1 RAM，GB3 预留 device。
        bo.uartKind = "pl011";
        bo.uartBase = 0x09000000;
        bo.uartShift = 0;
        bo.uartClk = 24000000;
        bo.baud = 115200;
        // 0x42000000：QEMU virt 把 DTB 压在 RAM 基址 0x40000000（约 1MB），
        // 载入基址避开（loader 设 PC 直跑，无需 Image 头）。
        bo.loadBase = 0x42000000;
        bo.stackSize = 1 << 20;
        bo.mapGb[0] = 0; // 1 = normal，0 = device
        bo.mapGb[1] = 1;
        bo.mapGb[2] = 1;
        bo.mapGb[3] = 0;
        (void)cpu; // 通用 armv8-a（QEMU 侧 -cpu 可任意）
        return "";
    }
    if (name == "rk3588") {
        // RK3588：DW APB 16550（reg-shift=2）@ uart2 0xfebb0000（板厂
        // 调试串口标准位），24MHz 源时钟，默认 1.5M 波特（RK 出厂约定）；
        // DRAM 自 0x00200000 起可用，载入基址取 0x00A00000（U-Boot go/
        // booti 常用落点）；GB0-2 = DRAM（normal），GB3 = 外设（device）。
        bo.uartKind = "16550";
        bo.uartBase = 0xfebb0000;
        bo.uartShift = 2;
        bo.uartClk = 24000000;
        bo.baud = 1500000;
        bo.loadBase = 0x00a00000;
        bo.stackSize = 1 << 20;
        bo.mapGb[0] = 1;
        bo.mapGb[1] = 1;
        bo.mapGb[2] = 1;
        bo.mapGb[3] = 0;
        if (cpu.empty()) cpu = "cortex-a76";
        return "";
    }
    return "未知板级预设 '" + name +
           "'（可用：qemu-virt-a64、rk3588；或 --uart-*/--load-base 手工指定）";
}

namespace {

/// lld undefined symbol → 能力域处方（裸机白名单的对外解释面）。
std::string bareSymbolHint(const std::string& sym) {
    struct Rule {
        const char* prefix;
        const char* domain;
    };
    static const Rule rules[] = {
        {"sg_try_", "try/except（裸机无宿主栈展开，白名单外）"},
        {"_setjmp", "try/except（裸机无宿主栈展开，白名单外）"},
        {"sg_read_text", "io"},
        {"sg_write_text", "io"},
        {"sg_append_text", "io"},
        {"sg_exists", "io"},
        {"sg_list_dir", "io"},
        {"sg_input", "io"},
        {"sg_data_ptr", "io"},
        {"sg_net_", "net"},
        {"sg_json_", "json"},
        {"sg_gui_", "gui"},
        {"sg_thread_", "thread"},
        {"sg_lock_", "thread"},
        {"sg_chan_", "thread"},
        {"sg_spawn_", "proc"},
        {"sg_system", "proc"},
        {"sg_env", "env"},
        {"sg_has_env", "env"},
        {"sg_set_env", "env"},
        {"sg_unset_env", "env"},
        {"sg_dict_", "Dict（裸机白名单：仅 List/Result）"},
        {"sg_set_", "Set（裸机白名单：仅 List/Result）"},
        {"sg_tensor_", "Tensor"},
        {"sg_sha256", "sha256"},
        {"sg_sin_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_cos_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_tan_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_asin_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_acos_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_atan", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_sinh_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_cosh_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_tanh_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_exp_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_log_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_pow_", "数学函数（裸机白名单：sqrt/fabs/floor/ceil/round/trunc/fmod）"},
        {"sg_epoch", "epoch/localtime/gmtime（裸机无 RTC；now() 可用）"},
        {"sg_localtime", "epoch/localtime/gmtime（裸机无 RTC；now() 可用）"},
        {"sg_gmtime", "epoch/localtime/gmtime（裸机无 RTC；now() 可用）"},
        {"sg_sorted", "sorted（裸机白名单外）"},
        {"sg_list_reversed", "reversed（裸机白名单外）"},
        {"sg_str_reversed", "reversed（裸机白名单外）"},
        {"sg_extern", "FFI（裸机无 dlopen；C 依赖须随固件静态链接）"},
    };
    for (const Rule& r : rules) {
        if (sym.rfind(r.prefix, 0) == 0) return r.domain;
    }
    return "sg_bare 白名单外";
}

/// 链接失败输出 → 处方诊断（解析 undefined symbol 行，最多列 5 个）。
std::string mapBareLinkErrors(const std::string& toolOut) {
    std::string out = "构建中止：裸机链接失败——以下符号不在 sg_bare 白名单：\n";
    int shown = 0;
    size_t pos = 0;
    while (shown < 5) {
        size_t p = toolOut.find("undefined symbol: ", pos);
        if (p == std::string::npos) break;
        p += strlen("undefined symbol: ");
        size_t e = p;
        while (e < toolOut.size() && toolOut[e] != '\n' &&
               toolOut[e] != '\r' && toolOut[e] != ' ')
            ++e;
        std::string sym = toolOut.substr(p, e - p);
        out += "  " + sym + " → " + bareSymbolHint(sym) + "\n";
        pos = e;
        ++shown;
    }
    if (shown == 0) out = "构建中止：裸机链接失败\n";
    return out;
}

void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream f(p, std::ios::binary);
    f << content;
}

} // namespace

BuildOutcome buildBareOnce(const fs::path& inPath, const fs::path& outPath,
                           bool gpuMode, bool useCache, const CrossOpts& xo) {
    (void)useCache; // v1 无 obj 缓存（产物小；后续并入统一缓存体系）
    BuildOutcome b;
    b.exe = outPath;
    const std::string entry = inPath.stem().string();

    // ---- 目标校验：v1 仅 aarch64（Cortex-M 模板后续批次） ----
    if (xo.target.rfind("aarch64", 0) != 0) {
        b.log = "构建中止：裸机 crt 模板当前仅支持 aarch64 目标（"
                "armv7m-none-eabi 等在后续批次）；实得 target='" +
                xo.target + "'\n";
        b.exitCode = 1;
        return b;
    }
    if (gpuMode) {
        b.log = "构建中止：裸机目标不支持 --gpu（无宿主 GPU 语义）\n";
        b.exitCode = 1;
        return b;
    }
    if (!xo.sysroot.empty()) {
        b.log = "构建中止：裸机目标不使用 --sysroot（无 libc；crt 三件套内置）\n";
        b.exitCode = 1;
        return b;
    }

    // ---- 工具链 ----
    fs::path llvmBin = bshared::llvmBinDir();
    if (llvmBin.empty()) {
        b.log = "构建中止：未找到 clang（SCOURGE_LLVM_BIN 可指定 LLVM bin 目录）\n";
        b.exitCode = 1;
        return b;
    }
    fs::path clang = llvmBin /
#ifdef _WIN32
        "clang.exe";
#else
        "clang";
#endif
    fs::path lld = llvmBin /
#ifdef _WIN32
        "lld.exe"; // 通用驱动：-flavor gnu 选 ELF 后端
#else
        "ld.lld";
#endif
    fs::path objcopy = llvmBin /
#ifdef _WIN32
        "llvm-objcopy.exe";
#else
        "llvm-objcopy";
#endif

    // ---- 前端（与宿主同管线）：加载 → 检查 → MLIR → LLVM IR ----
    Session session;
    if (!session.load(inPath)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    session.check();
    if (session.diag().hasErrors()) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    auto order = session.modulesInDependencyOrder(entry);
    uint64_t sessionCaps = 0;
    for (const auto& name : order) {
        const Module* m = session.loadedModule(name);
        if (m) sessionCaps |= capMaskOf(*m);
    }
    std::string mlir;
    if (!aot::generateMLIR(session, entry, mlir)) {
        session.diag().emitText(b.log);
        b.exitCode = 1;
        return b;
    }
    fs::path dir = outPath.parent_path();
    if (dir.empty()) dir = ".";
    std::string stem = outPath.stem().string();
    fs::path llPath = dir / (stem + ".ll");
    fs::path capPath = dir / (stem + ".buildcap");
    std::string toolOut;
    if (!aot::convertToLLVMInProcess(mlir, llPath.string(), toolOut, false,
                                     sessionCaps)) {
        std::ofstream cap(capPath, std::ios::binary);
        cap << toolOut;
        b.log = "构建中止：MLIR → LLVM IR 进程内转换失败\n" + toolOut;
        b.exitCode = 1;
        return b;
    }

    // ---- crt 三件套展开（板级参数 → @TOKEN@） ----
    const BareOpts& bo = xo.bare;
    std::string flags =
        " --target=" + xo.target + " -ffreestanding -fno-builtin "
        "-fno-stack-protector -fno-asynchronous-unwind-tables -O2";
    if (!xo.cpu.empty()) flags += " -mcpu=" + xo.cpu;
    if (!xo.march.empty()) flags += " -march=" + xo.march;

    std::string crtC = bare::detok(bare::kA64CrtC, "UART_KIND",
                                   bo.uartKind == "pl011" ? "1" : "0");
    {
        char buf[32];
        snprintf(buf, sizeof buf, "0x%llx",
                 static_cast<unsigned long long>(bo.uartBase));
        crtC = bare::detok(crtC.c_str(), "UART_BASE", buf);
        snprintf(buf, sizeof buf, "%d", bo.uartShift);
        crtC = bare::detok(crtC.c_str(), "UART_SHIFT", buf);
        snprintf(buf, sizeof buf, "%llu",
                 static_cast<unsigned long long>(bo.uartClk));
        crtC = bare::detok(crtC.c_str(), "UART_CLK", buf);
        snprintf(buf, sizeof buf, "%d", bo.baud);
        crtC = bare::detok(crtC.c_str(), "UART_BAUD", buf);
        for (int gb = 0; gb < 4; ++gb) {
            snprintf(buf, sizeof buf, "GB%d", gb);
            crtC = bare::detok(crtC.c_str(), buf,
                               std::to_string(bo.mapGb[gb] ? 0 : 1));
            // mapGb：1=normal；模板 SG_GBn：1=device
        }
    }
    std::string ldScript;
    {
        char buf[32];
        snprintf(buf, sizeof buf, "0x%llx",
                 static_cast<unsigned long long>(bo.loadBase));
        ldScript = bare::detok(bare::kA64LdT, "LOAD_BASE", buf);
        snprintf(buf, sizeof buf, "%llu",
                 static_cast<unsigned long long>(bo.stackSize));
        ldScript = bare::detok(ldScript.c_str(), "STACK_SIZE", buf);
    }

    fs::path bootPath = dir / (stem + "_crt.S");
    fs::path crtPath = dir / (stem + "_crt.c");
    fs::path rtPath = dir / (stem + "_rt.c");
    fs::path ldPath = dir / (stem + ".ld");
    fs::path binPath = outPath;
    binPath.replace_extension(".bin");
    b.bin = binPath;
    writeFile(bootPath, bare::kA64BootS);
    writeFile(crtPath, crtC);
    writeFile(rtPath, std::string(bare::kBareRtC) + bare::kBareRtStrC +
                          bare::kBareRtFmtC + bare::kBareRtStrTailC);
    writeFile(ldPath, ldScript);

    // ---- 编译四件（main.ll / entry.S / crt / rt）+ 链接 + objcopy ----
    auto cap = [&](const std::string& exe, const std::string& args) {
        return bshared::runCapture(exe, args, capPath, toolOut);
    };
    auto compile = [&](const fs::path& src, const fs::path& obj,
                       bool isLl) -> bool {
        std::string args = "-c -Wno-override-module" + flags + " \"" +
                           src.string() + "\" -o \"" + obj.string() + "\"";
        (void)isLl;
        int rc = cap(clang.string(), args);
        if (rc != 0) {
            b.log = "构建中止：clang 编译 " + src.string() +
                    " 失败（exit " + std::to_string(rc) + "）\n" + toolOut;
            b.exitCode = 1;
            return false;
        }
        return true;
    };

    fs::path objMain = dir / (stem + "_main.o");
    fs::path objBoot = dir / (stem + "_boot.o");
    fs::path objCrt = dir / (stem + "_crt.o");
    fs::path objRt = dir / (stem + "_rt.o");
    if (!compile(llPath, objMain, true)) return b;
    if (!compile(bootPath, objBoot, false)) return b;
    if (!compile(crtPath, objCrt, false)) return b;
    if (!compile(rtPath, objRt, false)) return b;

    std::string linkArgs =
#ifdef _WIN32
        "-flavor gnu ";
#else
        "";
#endif
    linkArgs += "-T \"" + ldPath.string() + "\" \"" + objBoot.string() +
                "\" \"" + objMain.string() + "\" \"" + objCrt.string() +
                "\" \"" + objRt.string() + "\" -o \"" + outPath.string() + "\"";
    if (cap(lld.string(), linkArgs) != 0) {
        b.log = mapBareLinkErrors(toolOut);
        if (toolOut.find("undefined symbol") == std::string::npos)
            b.log += toolOut;
        b.exitCode = 1;
        return b;
    }
    if (cap(objcopy.string(),
            "-O binary \"" + outPath.string() + "\" \"" + binPath.string() +
                "\"") != 0) {
        // bin 失败不致命（ELF 已产出；Renode LoadELF 亦可用）
        b.log = "警告：llvm-objcopy 生成 bin 失败（ELF 已产出）\n" + toolOut;
        b.bin.clear();
    }
    return b; // exitCode 0
}

} // namespace scourge
