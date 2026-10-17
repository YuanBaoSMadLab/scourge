// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 栈式虚拟机实现（SG-BC 解释器）— 驱动：生命周期与顶层编排
// ----------------------------------------------------------------------------
//  语义纪律（与 AOT 共享前端，双模不漂移）：
//    * '/' 真除法恒为 float；'//' 与 '%' 保持 int（Python 语义）；
//    * 整数加/减/乘/幂/取负溢出、除零、索引越界、断言失败、unwrap 失败态
//      一律 panic（可被 try/except 捕获），无 UB；
//    * panic 经运行时 catch 栈回退：帧栈截断到捕获帧、操作数栈回退到
//      EnterTry 时的深度、pc 置于 handler（编译器已 patch）。
//  分块：值辅助见 include/scourge/BC/VMDetail.hpp；内建/分派/运算见
//  同目录 VMBuiltin/VMDispatch/VMOps.cpp；成员方法/网络/系统/JSON/@extern
//  见 VMMethod/VMNet/VMSys/VMJson/VMExtern.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include "scourge/Common/Capability.hpp"

namespace scourge {
namespace bc {

// ----------------------------------------------------------------------------
// 字典（DictData）：插入序哈希表。键哈希/相等与 valuesEqual 语义一致
// （数值键统一按 double 位哈希，int 1 与 float 1.0 同键；str 用 FNV-1a）。
// ----------------------------------------------------------------------------

static uint64_t dictMix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static uint64_t dictHashKey(const Value& k) {
    switch (k.kind) {
    case Value::Kind::Int:
    case Value::Kind::Float:
    case Value::Kind::Bool: {
        double d = asDouble(k);
        if (d == 0.0) d = 0.0; // -0.0 与 0.0 同键
        uint64_t bits = 0;
        std::memcpy(&bits, &d, sizeof(bits));
        return dictMix64(bits);
    }
    case Value::Kind::Str: {
        uint64_t h = 14695981039346656037ull;
        for (unsigned char c : k.s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return dictMix64(h);
    }
    default:
        return dictMix64(0); // 检查器已限标量键；None 等兜底同桶链
    }
}

uint32_t DictData::find(const Value& key) const {
    uint64_t h = dictHashKey(key);
    for (uint32_t e = buckets[h % buckets.size()]; e != kEmpty;
         e = entries[e].next)
        if (valuesEqual(entries[e].key, key)) return e;
    return kEmpty;
}

bool DictData::set(const Value& key, const Value& val) {
    if (uint32_t e = find(key); e != kEmpty) {
        entries[e].val = val;
        return false;
    }
    if ((entries.size() + 1) * 4 > buckets.size() * 3) { // 装载因子 > 3/4 扩容
        buckets.assign(buckets.size() * 2, kEmpty);
        for (size_t k = 0; k < entries.size(); ++k) {
            uint64_t h = dictHashKey(entries[k].key);
            entries[k].next = buckets[h % buckets.size()];
            buckets[h % buckets.size()] = static_cast<uint32_t>(k);
        }
    }
    uint64_t h = dictHashKey(key);
    Entry e;
    e.key = key;
    e.val = val;
    e.next = buckets[h % buckets.size()];
    buckets[h % buckets.size()] = static_cast<uint32_t>(entries.size());
    entries.push_back(std::move(e));
    return true;
}

// ----------------------------------------------------------------------------
// 集合（SetData）：仅键的插入序哈希表。链表结构与 DictData 同构，哈希/
// 相等复用字典键路径（dictHashKey/valuesEqual），元素语义与 Dict 键一致。
// ----------------------------------------------------------------------------

uint32_t SetData::find(const Value& key) const {
    uint64_t h = dictHashKey(key);
    for (uint32_t e = buckets[h % buckets.size()]; e != kEmpty;
         e = links[e])
        if (valuesEqual(entries[e], key)) return e;
    return kEmpty;
}

bool SetData::add(const Value& key) {
    if (find(key) != kEmpty) return false;
    if ((entries.size() + 1) * 4 > buckets.size() * 3) { // 装载因子 > 3/4 扩容
        buckets.assign(buckets.size() * 2, kEmpty);
        for (size_t k = 0; k < entries.size(); ++k) {
            uint64_t h = dictHashKey(entries[k]);
            links[k] = buckets[h % buckets.size()];
            buckets[h % buckets.size()] = static_cast<uint32_t>(k);
        }
    }
    uint64_t h = dictHashKey(key);
    uint32_t idx = static_cast<uint32_t>(entries.size());
    links.push_back(buckets[h % buckets.size()]);
    buckets[h % buckets.size()] = idx;
    entries.push_back(key);
    return true;
}

// ----------------------------------------------------------------------------
// Program 查询
// ----------------------------------------------------------------------------

int Program::findFunc(const std::string& module, const std::string& name) const {
    for (size_t i = 0; i < funcs.size(); ++i) {
        const FuncBC& f = funcs[i];
        if (f.isBuiltin) continue;
        if (f.module == module && f.name == name) return static_cast<int>(i);
    }
    return -1;
}

std::string stringify(const Value& v) { return toString(v, false); }

// ----------------------------------------------------------------------------
// 生命周期与顶层执行
// ----------------------------------------------------------------------------

VM::VM(Program program, VMOptions opts, std::shared_ptr<ThreadWorld> world)
    : program_(std::move(program)), opts_(std::move(opts)),
      world_(world ? std::move(world) : std::make_shared<ThreadWorld>()) {
    // globals_ 恒指向 world_->globals：worker 与主实例逐槽共享（GIL 互斥）；
    // 单线程路径多一层间接，语义逐字节不变。
    globals_ = world_->globals;
}

bool VM::run(const std::vector<std::string>& modules, const std::string& entry) {
    globals_->assign(program_.globalNames.size(), Value::makeNone());
    frames_.clear();
    catches_.clear();
    lastReturn_ = Value::makeNone();
    panicked_ = false;
    exitCode_ = 0;

    // 各模块 <top>：全局初始化（modules 已是依赖层序，被依赖者先初始化）。
    for (const auto& name : modules) {
        int t = program_.findFunc(name, "<top>");
        if (t < 0) continue;
        callFunc(t, 0);
        execute();
        if (panicked_) return false;
    }

    // 入口函数：int 返回值即退出码（REPL 求值包装经 entryFunc 指定，
    // 非 int 返回时 exitCode 取 0，不影响 REPL）。
    const std::string entryName =
        opts_.entryFunc.empty() ? "main" : opts_.entryFunc;
    int mi = program_.findFunc(entry, entryName);
    if (mi >= 0) {
        callFunc(mi, 0);
        execute();
        if (panicked_) return false;
        exitCode_ = lastReturn_.kind == Value::Kind::Int
                        ? static_cast<int>(lastReturn_.i)
                        : 0;
    }
    // main 结束：隐式 join 本实例 spawn 且未显式 join 的全部线程（M1 处方；
    // panic 路径不 join——进程即将终止，与 AOT sg_panic exit(1) 同语义）。
    joinOwnThreads();
    return true;
}

void VM::execute() {
    // GIL（真并发 M1）：RAII 持有整个执行期——panic 异常展开自动释放；
    // 阻塞内建（join/chan_recv/lock_acquire）由当前线程自持自放。
    std::unique_lock<std::mutex> gil(world_->gil);
    while (!frames_.empty()) {
        try {
            runLoop();
        } catch (RuntimePanic& rp) {
            if (!recover(rp.panic)) {
                panicked_ = true;
                return;
            }
        }
    }
}

void VM::runLoop() {
    while (!frames_.empty()) {
        Frame& f = frames_.back();
        if (f.pc >= f.fn->code.size()) {
            doReturn(Value::makeNone()); // 编译器保证 ReturnNone 收尾；防御
            continue;
        }
        Instr in = f.fn->code[f.pc];
        ++f.pc;
        dispatch(in);
    }
}

// ----------------------------------------------------------------------------
// 操作数栈
// ----------------------------------------------------------------------------

Value VM::pop() {
    Frame& f = frames_.back();
    if (f.stack.empty()) panicHere("VM 内部错误：操作数栈下溢");
    Value v = std::move(f.stack.back());
    f.stack.pop_back();
    return v;
}

void VM::push(Value v) { frames_.back().stack.push_back(std::move(v)); }

void VM::doReturn(Value v) {
    // 帧入池（保留 locals/stack 容量）——深递归/调用密集场景不再反复堆分配。
    // 池上限取 maxDepth：帧总数受调用深度护栏约束，池不会无限增长。
    if (freeFrames_.size() < opts_.maxDepth)
        freeFrames_.push_back(std::move(frames_.back()));
    frames_.pop_back();
    // return 可能发生在 try 体内 → 清理随之失效的 catch 记录。
    while (!catches_.empty() && catches_.back().frameIndex >= frames_.size())
        catches_.pop_back();
    if (!frames_.empty())
        frames_.back().stack.push_back(std::move(v));
    else
        lastReturn_ = std::move(v);
}

// ----------------------------------------------------------------------------
// 调用
// ----------------------------------------------------------------------------

VM::Frame VM::takeFreeFrame() {
    Frame fr;
    if (!freeFrames_.empty()) {
        fr = std::move(freeFrames_.back());
        freeFrames_.pop_back();
    }
    fr.pc = 0;
    fr.stack.clear(); // 析构旧值（引用计数正确回落），缓冲容量保留
    return fr;
}

void VM::callFunc(int fidx, uint32_t argc) {
    const FuncBC& fn = program_.funcs[static_cast<size_t>(fidx)];

    if (fn.isBuiltin) {
        if (frames_.empty()) panicHere("VM 内部错误：内建函数无调用帧");
        doBuiltin(fn, argc);
        return;
    }

    // @extern（H 专项）：动态库调用，实参从调用栈弹出（实现分块 VMExtern.cpp）。
    if (fn.isExtern) {
        if (frames_.empty()) panicHere("VM 内部错误：extern 函数无调用帧");
        // FFI 运行时门禁（0.9.7）：同 doBuiltin 纵深防御，消息同 AOT。
        if (!(program_.capMask & capBit(Cap::Ffi)))
            panicHere(capPanicMsg(Cap::Ffi, fn.name));
        doExtern(fn, argc);
        return;
    }

    if (argc < fn.requiredArgs || argc > fn.paramCount)
        panicHere("函数 '" + displayName(fn) + "' 实参个数不符（需要 " +
                  std::to_string(fn.requiredArgs) + "~" +
                  std::to_string(fn.paramCount) + " 个，实际 " +
                  std::to_string(argc) + " 个）");
    if (!frames_.empty() && frames_.size() >= opts_.maxDepth)
        panicHere("调用深度超限（上限 " + std::to_string(opts_.maxDepth) +
                  " 层）");
    if (fn.code.empty())
        panicHere("函数 '" + displayName(fn) +
                  "' 无函数体（所属模块未编译进字节码）");

    if (frames_.empty()) {
        // 根调用（<top> / main 直启）：无调用者栈，实参只能来自缺省值。
        Frame fr = takeFreeFrame();
        fr.fn = &fn;
        fr.locals.resize(fn.nlocals);
        for (uint32_t k = argc; k < fn.paramCount; ++k) {
            uint32_t di = k - fn.requiredArgs;
            fr.locals[k] = di < fn.defaults.size()
                               ? fn.defaults[di]
                               : Value::makeNone();
        }
        frames_.push_back(std::move(fr));
        return;
    }

    Frame& caller = frames_.back();
    if (caller.stack.size() < argc) panicHere("VM 内部错误：实参栈下溢");

    Frame fr = takeFreeFrame();
    fr.fn = &fn;
    fr.locals.resize(fn.nlocals);
    size_t base = caller.stack.size() - argc;
    for (uint32_t k = 0; k < argc; ++k)
        fr.locals[k] = std::move(caller.stack[base + k]);
    caller.stack.resize(base);
    for (uint32_t k = argc; k < fn.paramCount; ++k) {
        uint32_t di = k - fn.requiredArgs;
        fr.locals[k] = di < fn.defaults.size() ? fn.defaults[di]
                                               : Value::makeNone();
    }
    frames_.push_back(std::move(fr)); // caller 引用自此失效
}

uint64_t VM::rngNext() {
    // splitmix64：与 kRuntimeC 的 sg_rng_next 实现逐位一致（单一算法语义，
    // 双模各自实现、差分对拍锁定）。状态在 world_（进程级共享，GIL 下访问）。
    world_->rngState += 0x9E3779B97F4A7C15ull;
    uint64_t z = world_->rngState;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ----------------------------------------------------------------------------
// panic 与恢复
// ----------------------------------------------------------------------------

void VM::panicHere(const std::string& msg) {
    Panic p;
    p.message = msg;
    if (!frames_.empty()) {
        const Frame& f = frames_.back();
        p.func = displayName(*f.fn);
        p.module = f.fn->module;
        uint32_t pc = f.pc > 0 ? f.pc - 1 : 0;
        for (const auto& entry : f.fn->lines) {
            if (entry.first > pc) break;
            p.line = entry.second;
        }
    }
    throw RuntimePanic{std::move(p)};
}

bool VM::recover(const Panic& p) {
    lastPanic_ = p; // 已捕获同样记录（诊断可观测）
    if (catches_.empty()) return false;
    CatchPoint cp = catches_.back();
    catches_.pop_back();
    if (cp.frameIndex >= frames_.size()) return false; // 防御：catch 栈损坏
    frames_.resize(cp.frameIndex + 1);                 // 丢弃更深的调用帧
    Frame& f = frames_.back();
    if (f.stack.size() > cp.stackDepth) f.stack.resize(cp.stackDepth);
    f.pc = cp.handlerPc;
    return true;
}

} // namespace bc
} // namespace scourge
