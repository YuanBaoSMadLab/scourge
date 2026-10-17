// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge VM — @extern 动态库调用
// ----------------------------------------------------------------------------
//  doExtern 单一入口：实参从调用栈弹出，经通用 x64 机器码 thunk 按 MS x64
//  ABI 调用外部函数，返回值按 FuncBC.externRet 读回压栈。
//  MS x64 位置分配规则：按参数位置 p 单流占槽（整型/浮点不独立计数），
//  混合签名 (int, double) → rcx + xmm1；变参浮点双写 GP 影子槽。
//  仅 Windows 宿主：POSIX 宿主走 AOT dlopen 路径（panic 见 doExtern 尾）。
// ============================================================================
#include "scourge/BC/VM.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace scourge {
namespace {

// 跨平台（0.9.4）：MS x64 机器码 thunk 仅 Windows 宿主可用——SysV ABI
// 的寄存器分配/变参寄存器保存区完全不同，属独立工程量级。POSIX 宿主
// 的 @extern 走 AOT 路径（sg_extern_resolve = dlopen，C ABI 天然匹配）；
// VM 侧运行时给出明确 panic 而非错误机器码。
#ifdef _WIN32

// ---------------------------------------------------------------------------
// frame 布局（thunk 与装载端共用）：
//   gpr[4]  @0x00  rcx/rdx/r8/r9 整型/指针参数
//   fpr[4]  @0x20  xmm0..xmm3 浮点参数（double 位型）
//   stack16 @0x40  第 5+ 参数（整型/浮点共享位置流，MS x64 栈参顺序）
//   retI    @0xC0  整型/指针/bool 返回（RAX）
//   retF    @0xC8  浮点返回（XMM0；无害写回，按类型读取）
// ---------------------------------------------------------------------------
struct ExternFrame {
    void* gpr[4];
    double fpr[4];
    int64_t stack[16];
    int64_t retI;
    double retF;
    int64_t alCount; ///< 变参调用方义务：AL = 所用 xmm 寄存器数（MS x64）
};

/// 解析缓存（dll!sym → 函数地址；cap 与 AOT sg_extern_resolve 一致）。
constexpr size_t kExternCap = 256;
std::vector<std::pair<std::string, void*>>& externCache() {
    static std::vector<std::pair<std::string, void*>> c;
    return c;
}

/// 缓存互斥：多 world 并发下"查—缺—解析—插入"需原子，防重复插入突破 cap。
std::mutex& externCacheMu() {
    static std::mutex m;
    return m;
}

// ---------------------------------------------------------------------------
// 通用 thunk（一次生成；调用约定：rcx=frame、rdx=目标函数）：
//   mov r10, rcx ; mov rax, rdx ; sub rsp, 0xA8
//   16×{ mov rax,[r10+0x40+k*8]; mov [rsp+0x20+k*8],rax }  ; 栈参
//   mov rcx,[r10+0x00] … mov r9,[r10+0x18]                 ; gpr
//   movsd xmm0..xmm3,[r10+0x20..0x38]                      ; fpr
//   call rax
//   mov [r10+0xC0],rax ; movsd [r10+0xC8],xmm0             ; 返回写回
//   add rsp, 0xA8 ; ret
// sub 0xA8 = 168（栈参 16×8 + shadow 32 + 对齐垫 8）：入口 rsp≡8 (mod 16)，
// 减后 rsp≡0 (mod 16)，满足 MS x64 call 对齐。0xA8 的低 4 位为 8 → disp8
// 全部落在 ±127 内（栈参槽 [rsp+0x20..0x98]、frame 槽 [r10+0x00..0xC8]）。
// ---------------------------------------------------------------------------
uint8_t* generateThunk() {
    LPVOID p = VirtualAlloc(nullptr, 4096,
                            MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    if (!p) return nullptr;
    uint8_t* c = static_cast<uint8_t*>(p);
    auto e8 = [&](uint8_t b) { *c++ = b; };
    auto e32 = [&](uint32_t v) {
        *c++ = static_cast<uint8_t>(v);
        *c++ = static_cast<uint8_t>(v >> 8);
        *c++ = static_cast<uint8_t>(v >> 16);
        *c++ = static_cast<uint8_t>(v >> 24);
    };
    auto en = [&](const uint8_t* bs, size_t n) {
        std::memcpy(c, bs, n);
        c += n;
    };
    static const uint8_t kPrologue[] = {0x4C, 0x8B, 0xD1,             // mov r10,rcx
                                        0x48, 0x8B, 0xC2,             // mov rax,rdx
                                        0x48, 0x81, 0xEC, 0xA8, 0x00,
                                        0x00, 0x00};                  // sub rsp,0xA8
    en(kPrologue, sizeof kPrologue);
    // frame 指针自保存：r10 是 volatile 寄存器，call 后会被被调函数改写，
    // 预先存入 [rsp+0xA0]（sub 后 rsp 相对；栈参区 [0x20..0x98] 之后的
    // 空闲槽，与 shadow space/栈参/返回地址均不重叠）。
    e8(0x4C); e8(0x89); e8(0x94); e8(0x24);                          // mov [rsp+disp32],r10
    e32(0xA0);
    // 栈参搬运用 disp32 寻址：frame 侧 [r10+0x40..0xB8] 与栈侧
    // [rsp+0x20..0x98] 均超 disp8 有符号上限（+127），disp8 会回绕成负
    // 偏移读写越界。暂存寄存器用 r11（volatile）——rax 存有目标函数地址，
    // 供 call rax 使用，不可占用。
    for (int k = 0; k < 16; ++k) {
        e8(0x4D); e8(0x8B); e8(0x9A);                     // mov r11,[r10+disp32]
        e32(static_cast<uint32_t>(0x40 + k * 8));
        e8(0x4C); e8(0x89); e8(0x9C); e8(0x24);           // mov [rsp+disp32],r11（REX.WR：r11 需 R 位）
        e32(static_cast<uint32_t>(0x20 + k * 8));
    }
    static const uint8_t kGpr[] = {0x49, 0x8B, 0x4A, 0x00,           // mov rcx,[r10]
                                   0x49, 0x8B, 0x52, 0x08,           // mov rdx,[r10+8]
                                   0x4D, 0x8B, 0x42, 0x10,           // mov r8,[r10+0x10]（r8 需 REX.R）
                                   0x4D, 0x8B, 0x4A, 0x18};          // mov r9,[r10+0x18]（r9 需 REX.R）
    en(kGpr, sizeof kGpr);
    for (int k = 0; k < 4; ++k) {                                     // movsd xmmN,[r10+0x20+k*8]
        e8(0xF2); e8(0x49); e8(0x0F); e8(0x10);
        e8(static_cast<uint8_t>(0x42 + k * 8));
        e8(static_cast<uint8_t>(0x20 + k * 8));
    }
    // 变参调用方义务（0.9.6 O 节）：AL = 所用 xmm 数。rax 此刻存有目标
    // 地址，先移入 r11（栈参搬运已结束、r11 空闲），再装 AL、call r11——
    // 对定参被调方 AL 不被读，无条件装载零分叉。
    static const uint8_t kAlSetup[] = {
        0x49, 0x89, 0xC3,          // mov r11,rax
        0x41, 0x8A, 0x82, 0xD0, 0x00,
        0x00, 0x00,                // mov al,[r10+0xD0]（disp32：0xD0 超 disp8 有符号上限，disp8 会回绕读错槽）
        0x41, 0xFF, 0xD3};         // call r11
    en(kAlSetup, sizeof kAlSetup);
    // r10 是 volatile 寄存器（被调方可任意改写）：call 前把 frame 指针存入
    // [rsp+0xA0]（栈参区末尾之后的空闲槽），call 后取回再写返回值。
    e8(0x4C); e8(0x8B); e8(0x94); e8(0x24); e32(0xA0);               // mov r10,[rsp+0xA0]
    e8(0x49); e8(0x89); e8(0x82); e32(0xC0);                         // mov [r10+0xC0],rax
    e8(0xF2); e8(0x49); e8(0x0F); e8(0x11); e8(0x82); e32(0xC8);     // movsd [r10+0xC8],xmm0
    static const uint8_t kEpilogue[] = {0x48, 0x81, 0xC4, 0xA8, 0x00,
                                        0x00, 0x00,                   // add rsp,0xA8
                                        0xC3};                        // ret
    en(kEpilogue, sizeof kEpilogue);
    // 刷新失败意味着取指可能读到未写入的旧内容，必须释放视为不可用。
    if (!FlushInstructionCache(GetCurrentProcess(), p, 4096)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return static_cast<uint8_t*>(p);
}

/// 进程级唯一桩：多 world 并发首次调用也只生成一份。call_once 保证原子初始化。
uint8_t* ensureThunk() {
    static std::once_flag once;
    static uint8_t* thunk = nullptr;
    std::call_once(once, [] { thunk = generateThunk(); });
    return thunk;
}

using ThunkFn = void (*)(ExternFrame*, void*);
#endif // _WIN32

} // namespace

namespace bc {

// ---------------------------------------------------------------------------
// doExtern：调用点分派入口（VM::callFunc）
// ---------------------------------------------------------------------------
void VM::doExtern(const FuncBC& fn, uint32_t argc) {
    std::vector<Value> a(argc);
    for (uint32_t k = argc; k-- > 0;) a[k] = pop();
#ifdef _WIN32
    // ---- 解析（缓存 cap 与 AOT 一致；panic 消息逐字一致） ----
    // symbol 别名（@extern(..., symbol=...)）：查符号/缓存键/panic 消息
    // 三处同用解析后的符号——与 AOT 降级层传入的 sym 操作数逐字一致
    //（BuilderRTSys.hpp extern_resolve 同构）。
    const std::string& sym = fn.externSym.empty() ? fn.name : fn.externSym;
    std::lock_guard<std::mutex> cacheLk(externCacheMu());
    auto& cache = externCache();
    std::string key = fn.externDll + "!" + sym;
    void* target = nullptr;
    for (const auto& e : cache) {
        if (e.first == key) {
            target = e.second;
            break;
        }
    }
    if (!target) {
        if (cache.size() >= kExternCap)
            panicHere("extern_call() 动态库缓存已满");
        HMODULE h = LoadLibraryA(fn.externDll.c_str());
        if (!h)
            panicHere("extern_call() 无法加载动态库: " + fn.externDll);
        FARPROC p = GetProcAddress(h, sym.c_str());
        if (!p)
            panicHere("extern_call() 符号未找到: " + fn.externDll + "!" + sym);
        target = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(p));
        cache.emplace_back(std::move(key), target);
    }

    // ---- thunk 就绪 ----
    uint8_t* thunk = ensureThunk();
    if (!thunk) panicHere("extern_call() 无法生成调用桩（VirtualAlloc 失败）");

    // ---- frame 装载 ----
    // 定参与变参合流：按参数位置 p 单流占槽（MS x64 位置分配规则，
    // 见文件头契约与四方实证）。浮点双写 gpr[p] 位模式 + fpr[p] 值：
    // 前者是变参被调方 va_arg 的权威读取来源，后者是定参/变参被调
    // 方读浮点值的寄存器槽；定参被调方不读 GP 影子槽，双写无害。
    // 第 5+ 参数按位置序入栈槽（浮点同为位模式）。
    ExternFrame fr;
    std::memset(&fr, 0, sizeof fr);
    int si = 0;
    auto pushStack = [&](int64_t bits) {
        if (si >= 16) panicHere("extern_call() 参数过多（上限 20 个）");
        fr.stack[si++] = bits;
    };
    int p = 0, xmmHigh = 0;
    for (const Value& v : a) {
        switch (v.kind) {
        case Value::Kind::Float: {
            int64_t bits;
            std::memcpy(&bits, &v.f, 8);
            if (p < 4) {
                fr.gpr[p] =
                    reinterpret_cast<void*>(static_cast<intptr_t>(bits));
                fr.fpr[p] = v.f;
                xmmHigh = p + 1;
            } else {
                pushStack(bits);
            }
            break;
        }
        case Value::Kind::Int:
            if (p < 4)
                fr.gpr[p] =
                    reinterpret_cast<void*>(static_cast<intptr_t>(v.i));
            else
                pushStack(v.i);
            break;
        case Value::Kind::Bool:
            if (p < 4)
                fr.gpr[p] = reinterpret_cast<void*>(
                    static_cast<intptr_t>(v.b ? 1 : 0));
            else
                pushStack(v.b ? 1 : 0);
            break;
        case Value::Kind::Str:
            if (p < 4)
                fr.gpr[p] = const_cast<char*>(v.s.c_str());
            else
                pushStack(static_cast<int64_t>(
                    reinterpret_cast<intptr_t>(v.s.c_str())));
            break;
        default:
            panicHere("VM 内部错误：extern 参数类型");
        }
        ++p;
    }
    fr.alCount = xmmHigh;

    // ---- 调用 + 返回读回 ----
    // fflush 缓冲统一（0.9.9，F-4）：调用前冲刷宿主 stdout——外部 CRT
    // （msvcrt.dll 等）的 stdout 缓冲与宿主独立，先落盘我方既有输出，
    // 交错序确定（AOT LangExternCallRewrite 同点同语义）。
    std::fflush(stdout);
    reinterpret_cast<ThunkFn>(thunk)(&fr, target);
    switch (fn.externRet) {
    case 'i':
        push(Value::makeInt(fr.retI));
        break;
    case 'f':
        push(Value::makeFloat(fr.retF));
        break;
    case 'b':
        push(Value::makeBool((fr.retI & 1) != 0));
        break;
    case 's': {
        const char* s = reinterpret_cast<const char*>(
            static_cast<intptr_t>(fr.retI));
        push(Value::makeStr(s ? std::string(s) : std::string()));
        break;
    }
    default:
        push(Value::makeNone());
        break;
    }
#else
    (void)fn;
    panicHere("extern_call() VM 侧调用桩仅支持 Windows 宿主（MS x64 ABI）；"
              "POSIX 宿主请用 AOT 构建（dlopen 路径原生可用）");
#endif
}

} // namespace bc

} // namespace scourge
