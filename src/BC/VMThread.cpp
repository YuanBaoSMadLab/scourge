// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  Scourge 字节码 VM — 真并发 M1：线程/锁/通道内建
// ----------------------------------------------------------------------------
//  职责边界：只实现 thread_spawn/thread_join/lock_*/channel_new/chan_*；
//  doBuiltin 白名单分派进来（VMBuiltin.cpp 尾段，本文件是唯一实现处）。
//  语义契约（与 include/scourge/Tool/BuilderRTThread.hpp 的 sg_thread_* /
//  sg_lock_* / sg_channel_new / sg_chan_* 逐字一致）：
//    * GIL（world_->gil）由 execute() 入口 RAII 持有；本文件内阻塞操作
//      （join/chan_recv 等待/lock_acquire）当前线程自持自放 GIL；
//    * 句柄 = world_ 句柄表 1 起始下标（0 = 空槽，join/close 后回收）；
//      全部注册表操作都在 GIL 持有下执行，无需独立注册表锁；
//    * thread_spawn 实参经 Sema 静态校验（字符串字面量 + 当前模块 0 参函数），
//      运行期查不到目标即 panic（字节码被篡改的纵深防御路径）；
//    * worker = 同 world 的新 VM 实例（globals/rng 共享，Program 拷贝为
//      M1 接受的代价）；threadEntry 收尾隐式 join 自己 spawn 的未 join 线程；
//    * worker panic → sg_panic 格式逐字打印 + exit(1)（无 try/except 边界）；
//    * chan 无界（send 永不阻塞），消息恒 str（拷贝入队，所有权移交接收方）；
//      空且 closed 时 recv panic；重复 close panic；
//    * 锁序纪律：恒 gil → Chan::m（见 VMDetail.hpp），不得反向。
//  其余阶段见同目录 VM*.cpp（拆分纪律见 AGENTS.md）。
// ============================================================================
#include "scourge/BC/VM.hpp"
#include "scourge/BC/VMDetail.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace scourge {
namespace bc {

namespace {

/// 句柄表容量上限（与 AOT BuilderRTThread.hpp 宏一致）。
constexpr size_t kMaxThreads = 256;
constexpr size_t kMaxLocks = 1024;
constexpr size_t kMaxChans = 1024;

} // namespace

void VM::threadEntry(int fidx) {
    callFunc(fidx, 0);
    execute();
    // execute 返回即 GIL 已释放；worker panic 无 catch 栈可依——按
    // sg_panic 格式逐字打印（双模消息一致，file 以 module 名代替：
    // VM 不持有源码路径映射，CLI 层不做 worker panic 对拍）。
    if (panicked_) {
        const Panic& p = lastPanic_;
        std::fprintf(stderr, "panic: %s at %s (%s:%u)\n", p.message.c_str(),
                     p.func.c_str(), p.module.c_str(), p.line);
        std::fflush(stderr);
        std::exit(1);
    }
    joinOwnThreads();
}

void VM::joinOwnThreads() {
    // 仅 join 本实例 spawn 且未被显式 join 的句柄（槽空 = 已 join）。
    for (int64_t h : myThreads_) {
        auto& slot = world_->threads[static_cast<size_t>(h) - 1];
        if (!slot) continue;
        std::thread* t = slot.get();
        world_->gil.unlock(); // join 阻塞期间放 GIL（worker 内建可推进）
        t->join();
        world_->gil.lock();
        slot.reset();
    }
    myThreads_.clear();
}

void VM::threadBuiltin(const std::string& n, std::vector<Value>& a) {
    // GIL 全程持有（execute() 入口 RAII 保证）；阻塞段按锁序纪律自持自放。
    if (n == "thread_spawn") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Str)
            panicHere("thread_spawn() 参数必须是 str（目标函数名）");
        if (world_->threads.size() >= kMaxThreads)
            panicHere("thread_spawn() 线程数已达上限");
        // 当前帧所属模块（Sema 已限定目标必须是本模块 0 参函数）。
        const std::string& mod = frames_.back().fn->module;
        int fidx = program_.findFunc(mod, a[0].s);
        if (fidx < 0)
            panicHere("thread_spawn() 未找到当前模块函数 '" + a[0].s + "'");
        // worker：同 world 新 VM（globals/rng 共享）。Program 拷贝为 M1
        // 接受的代价——单 Program 每线程一份，字节码只读无竞争。
        VM* w = new VM(program_, opts_, world_);
        // 空槽复用 + 登记（在 GIL 下完成，新线程尚无机会动表）。
        size_t slot = 0;
        for (size_t k = 0; k < world_->threads.size(); ++k) {
            if (!world_->threads[k]) { slot = k + 1; break; }
        }
        int64_t h;
        if (slot) {
            h = static_cast<int64_t>(slot);
            world_->threads[slot - 1] = std::make_unique<std::thread>(
                [w, fidx] { w->threadEntry(fidx); delete w; });
        } else {
            h = static_cast<int64_t>(world_->threads.size()) + 1;
            world_->threads.push_back(std::make_unique<std::thread>(
                [w, fidx] { w->threadEntry(fidx); delete w; }));
        }
        myThreads_.push_back(h); // 隐式 join 清单（显式 join 后清除）
        push(Value::makeInt(h));
        return;
    }
    if (n == "thread_join") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Int)
            panicHere("thread_join() 参数必须是 int");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->threads.size() ||
            !world_->threads[static_cast<size_t>(h) - 1])
            panicHere("thread_join() 无效的线程句柄");
        std::thread* t = world_->threads[static_cast<size_t>(h) - 1].get();
        world_->gil.unlock();
        t->join();
        world_->gil.lock();
        world_->threads[static_cast<size_t>(h) - 1].reset();
        // 从隐式 join 清单移除（避免收尾重复处理——槽空已跳过，此为簿记）。
        for (size_t k = 0; k < myThreads_.size(); ++k) {
            if (myThreads_[k] == h) {
                myThreads_.erase(myThreads_.begin() +
                                 static_cast<std::ptrdiff_t>(k));
                break;
            }
        }
        push(Value::makeNone());
        return;
    }
    if (n == "lock_new") {
        if (!a.empty()) panicHere("lock_new() 不需要参数");
        if (world_->locks.size() >= kMaxLocks)
            panicHere("lock_new() 锁数已达上限");
        world_->locks.push_back(std::make_shared<std::mutex>());
        push(Value::makeInt(static_cast<int64_t>(world_->locks.size())));
        return;
    }
    if (n == "lock_acquire") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Int)
            panicHere("lock_acquire() 参数必须是 int");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->locks.size())
            panicHere("lock_acquire() 无效的锁句柄");
        std::mutex& m = *world_->locks[static_cast<size_t>(h) - 1];
        world_->gil.unlock();
        m.lock();
        world_->gil.lock();
        push(Value::makeNone());
        return;
    }
    if (n == "lock_release") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Int)
            panicHere("lock_release() 参数必须是 int");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->locks.size())
            panicHere("lock_release() 无效的锁句柄");
        // release 不动 GIL（持锁段仍继续持有 GIL，直到 execute 退出或下一
        // 阻塞点——锁语义与 GIL 语义正交）。
        world_->locks[static_cast<size_t>(h) - 1]->unlock();
        push(Value::makeNone());
        return;
    }
    if (n == "channel_new") {
        if (!a.empty()) panicHere("channel_new() 不需要参数");
        if (world_->chans.size() >= kMaxChans)
            panicHere("channel_new() 通道数已达上限");
        world_->chans.push_back(std::make_shared<Chan>());
        push(Value::makeInt(static_cast<int64_t>(world_->chans.size())));
        return;
    }
    if (n == "chan_send") {
        if (a.size() != 2 || a[1].kind != Value::Kind::Str)
            panicHere("chan_send() 参数必须是 (int, str)");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->chans.size())
            panicHere("chan_send() 无效的通道句柄");
        Chan& c = *world_->chans[static_cast<size_t>(h) - 1];
        std::lock_guard<std::mutex> cl(c.m);
        if (c.closed) panicHere("chan_send() 通道已关闭");
        c.q.push_back(a[1].s); // 拷贝入队（发送方后续修改互不可见）
        c.cv.notify_one();
        push(Value::makeNone());
        return;
    }
    if (n == "chan_recv") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Int)
            panicHere("chan_recv() 参数必须是 int");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->chans.size())
            panicHere("chan_recv() 无效的通道句柄");
        Chan& c = *world_->chans[static_cast<size_t>(h) - 1];
        std::unique_lock<std::mutex> cl(c.m);
        while (c.q.empty() && !c.closed) {
            // 等待期放 GIL（其他线程可推进）；cv.wait 原子放 c.m 睡眠。
            // 醒来必须先放 c.m 再取 gil——锁序恒 gil → c.m（VMDetail 纪律）：
            // 持 c.m 等 gil 会与"持 gil 取 c.m 的 chan_send"构成 ABBA 死锁
            // （多消息连发时 sender 持 gil 期间反复取 c.m，必现）。
            world_->gil.unlock();
            c.cv.wait(cl);
            cl.unlock();
            world_->gil.lock();
            cl.lock();
            // 此刻 gil 在手，任何 send/close 都推进不了，条件复查即终态。
        }
        if (c.q.empty())
            panicHere("chan_recv() 通道已关闭且无消息");
        std::string msg = std::move(c.q.front());
        c.q.pop_front();
        cl.unlock();
        push(Value::makeStr(std::move(msg))); // 所有权移交接收方
        return;
    }
    if (n == "chan_close") {
        if (a.size() != 1 || a[0].kind != Value::Kind::Int)
            panicHere("chan_close() 参数必须是 int");
        int64_t h = a[0].i;
        if (h < 1 || static_cast<size_t>(h) > world_->chans.size())
            panicHere("chan_close() 无效的通道句柄");
        Chan& c = *world_->chans[static_cast<size_t>(h) - 1];
        std::lock_guard<std::mutex> cl(c.m);
        if (c.closed) panicHere("chan_close() 通道已关闭");
        c.closed = true;
        c.cv.notify_all(); // 唤醒全部等待者：有消息取消息，无消息 panic
        push(Value::makeNone());
        return;
    }
    panicHere("VM 内部错误：未知并发内建 '" + n + "'");
}

} // namespace bc
} // namespace scourge
