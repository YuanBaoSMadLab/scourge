// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  AOT C 运行时片段 — 真并发 M1：线程/锁/通道内建
// ----------------------------------------------------------------------------
//  职责边界：与 BuilderRTNet.hpp 同模式——build 时拼接在 kRuntimeC 之后落盘
//  （复用其 sg_panic/sg_io_msg/windows.h 定义），缓存键已计入本片段文本。
//  语义契约（与 src/BC/VMThread.cpp 逐字一致，双模差分由 verify 探针锁定）：
//    * 句柄 = 1 起始的静态表下标（join/close 置 NULL 空槽，可复用）；
//    * sg_thread_spawn 收函数指针（func.constant 取自 sg_target），worker
//      即裸 C 线程；panic 直接 sg_panic → exit(1)（与 VM threadEntry 同）；
//    * chan 无界（send 永不阻塞），消息 sg_dup 深拷贝入队，recv 移交所有权；
//    * 锁序：AOT 无 GIL——Chan 用自身互斥量（单锁自洽，无锁序约束）；
//    * 跨平台：Windows 用 _beginthreadex/CRITICAL_SECTION/CONDITION_VARIABLE，
//      POSIX 用 pthread_*（链接 -lpthread，由 Builder 链接命令提供）。
//  本文件常量字符串含中文（panic 消息对齐 VM），cl /utf-8 由 Builder 保证。
// ============================================================================
#pragma once

inline const char kThreadRuntimeC[] = R"TT(/* Scourge AOT 运行时 — 真并发片段（M1；语义对齐 VMThread.cpp）。 */
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <string.h>
#endif
#include <stdint.h>
#include <stdlib.h>

#define SG_THREAD_MAX 256
#define SG_LOCK_MAX 1024
#define SG_CHAN_MAX 1024

typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION m;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t m;
    pthread_cond_t cv;
#endif
    int closed;
    char** q;      /* 消息队列（sg_dup 深拷贝；recv 移交所有权） */
    size_t n, cap, head;
} SgChan;

static void sg_chan_init(SgChan* c) {
    c->closed = 0;
    c->q = NULL;
    c->n = 0;
    c->cap = 0;
    c->head = 0;
#ifdef _WIN32
    InitializeCriticalSection(&c->m);
    InitializeConditionVariable(&c->cv);
#else
    pthread_mutex_init(&c->m, NULL);
    pthread_cond_init(&c->cv, NULL);
#endif
}

static void sg_chan_push(SgChan* c, char* msg) {
    if (c->n == c->cap) {
        /* 环形满则重排（head 归零）+ 倍增；send 在锁内调用，无并发读写。 */
        size_t ncap = c->cap ? c->cap * 2 : 16;
        char** nq = (char**)malloc(ncap * sizeof(char*));
        if (!nq) exit(1);
        for (size_t i = 0; i < c->n; ++i)
            nq[i] = c->q[(c->head + i) % c->cap];
        free(c->q);
        c->q = nq;
        c->cap = ncap;
        c->head = 0;
    }
    c->q[(c->head + c->n) % c->cap] = msg;
    ++c->n;
}

static char* sg_chan_pop(SgChan* c) {
    char* m = c->q[c->head];
    c->head = (c->head + 1) % c->cap;
    --c->n;
    return m;
}

#ifdef _WIN32
typedef CRITICAL_SECTION SgMutex;
static void sg_mutex_init(SgMutex* m) { InitializeCriticalSection(m); }
static void sg_mutex_lock(SgMutex* m) { EnterCriticalSection(m); }
static void sg_mutex_unlock(SgMutex* m) { LeaveCriticalSection(m); }
static void sg_chan_wait(SgChan* c) { SleepConditionVariableCS(&c->cv, &c->m, INFINITE); }
static void sg_chan_notify_one(SgChan* c) { WakeConditionVariable(&c->cv); }
static void sg_chan_notify_all(SgChan* c) { WakeAllConditionVariable(&c->cv); }
typedef HANDLE SgThreadHandle;
static unsigned __stdcall sg_thr_entry(void* arg) {
    ((void (*)(void))(uintptr_t)arg)();
    return 0;
}
#else
typedef pthread_mutex_t SgMutex;
static void sg_mutex_init(SgMutex* m) { pthread_mutex_init(m, NULL); }
static void sg_mutex_lock(SgMutex* m) { pthread_mutex_lock(m); }
static void sg_mutex_unlock(SgMutex* m) { pthread_mutex_unlock(m); }
static void sg_chan_wait(SgChan* c) { pthread_cond_wait(&c->cv, &c->m); }
static void sg_chan_notify_one(SgChan* c) { pthread_cond_signal(&c->cv); }
static void sg_chan_notify_all(SgChan* c) { pthread_cond_broadcast(&c->cv); }
typedef pthread_t SgThreadHandle;
static void* sg_thr_entry(void* arg) {
    ((void (*)(void))(uintptr_t)arg)();
    return NULL;
}
#endif

static SgThreadHandle sg_thr_table[SG_THREAD_MAX];  /* 0/invalid = 空槽 */
static SgMutex* sg_lk_table[SG_LOCK_MAX];
static SgChan* sg_chan_table[SG_CHAN_MAX];

/* 句柄校验（1 起始；槽空 = 已 join/close/越界）；失败 panic（对齐 VM）。 */
static SgMutex* sg_lk_of(int64_t h, const char* fn, const char* file,
                         int64_t line, const char* call) {
    if (h < 1 || h > SG_LOCK_MAX || !sg_lk_table[h - 1])
        sg_panic(fn, file, line, sg_io_msg(call, "() 无效的锁句柄"));
    return sg_lk_table[h - 1];
}
static SgChan* sg_chan_of(int64_t h, const char* fn, const char* file,
                          int64_t line, const char* call) {
    if (h < 1 || h > SG_CHAN_MAX || !sg_chan_table[h - 1])
        sg_panic(fn, file, line, sg_io_msg(call, "() 无效的通道句柄"));
    return sg_chan_table[h - 1];
}

int64_t sg_thread_spawn(const char* name, void (*entry)(void),
                        const char* fn, const char* file, int64_t line) {
    (void)name;
    for (int64_t k = 0; k < SG_THREAD_MAX; ++k) {
        SgThreadHandle* slot = &sg_thr_table[k];
#ifdef _WIN32
        if (*slot) continue;
        uintptr_t th = _beginthreadex(NULL, 0, sg_thr_entry,
                                      (void*)(uintptr_t)entry, 0, NULL);
        if (!th) continue;
        *slot = (HANDLE)th;
        return k + 1;
#else
        pthread_t t;
        if (pthread_create(&t, NULL, sg_thr_entry,
                           (void*)(uintptr_t)entry) != 0)
            continue;
        *slot = t;
        return k + 1;
#endif
    }
    sg_panic(fn, file, line, "thread_spawn() 线程数已达上限");
    return 0; /* 不可达（sg_panic 不返回） */
}

void sg_thread_join(int64_t h, const char* fn, const char* file,
                    int64_t line) {
    if (h < 1 || h > SG_THREAD_MAX) goto bad;
#ifdef _WIN32
    if (!sg_thr_table[h - 1]) goto bad;
    WaitForSingleObject(sg_thr_table[h - 1], INFINITE);
    CloseHandle(sg_thr_table[h - 1]);
    sg_thr_table[h - 1] = NULL;
#else
    if (!sg_thr_table[h - 1]) goto bad;
    pthread_join(sg_thr_table[h - 1], NULL);
    sg_thr_table[h - 1] = (pthread_t)0;
#endif
    return;
bad:
    sg_panic(fn, file, line, "thread_join() 无效的线程句柄");
}

/* 满表 panic（消息与 VM 逐字一致——sg_lk_of 的句柄 0 分支仅为纵深防御）。 */
int64_t sg_lock_new(const char* fn, const char* file, int64_t line) {
    for (int64_t k = 0; k < SG_LOCK_MAX; ++k) {
        if (sg_lk_table[k]) continue;
        sg_lk_table[k] = (SgMutex*)malloc(sizeof(SgMutex));
        if (!sg_lk_table[k]) exit(1);
        sg_mutex_init(sg_lk_table[k]);
        return k + 1;
    }
    sg_panic(fn, file, line, "lock_new() 锁数已达上限");
    return 0;
}

void sg_lock_acquire(int64_t h, const char* fn, const char* file,
                     int64_t line) {
    sg_mutex_lock(sg_lk_of(h, fn, file, line, "lock_acquire"));
}

void sg_lock_release(int64_t h, const char* fn, const char* file,
                     int64_t line) {
    sg_mutex_unlock(sg_lk_of(h, fn, file, line, "lock_release"));
}

int64_t sg_channel_new(const char* fn, const char* file, int64_t line) {
    for (int64_t k = 0; k < SG_CHAN_MAX; ++k) {
        if (sg_chan_table[k]) continue;
        sg_chan_table[k] = (SgChan*)malloc(sizeof(SgChan));
        if (!sg_chan_table[k]) exit(1);
        sg_chan_init(sg_chan_table[k]);
        return k + 1;
    }
    sg_panic(fn, file, line, "channel_new() 通道数已达上限");
    return 0;
}

void sg_chan_send(int64_t h, const char* msg, const char* fn,
                  const char* file, int64_t line) {
    SgChan* c = sg_chan_of(h, fn, file, line, "chan_send");
    sg_mutex_lock(&c->m);
    if (c->closed) {
        sg_mutex_unlock(&c->m);
        sg_panic(fn, file, line, "chan_send() 通道已关闭");
    }
    sg_chan_push(c, sg_dup(msg)); /* 深拷贝入队（发送方后续修改互不可见） */
    sg_chan_notify_one(c);
    sg_mutex_unlock(&c->m);
}

char* sg_chan_recv(int64_t h, const char* fn, const char* file,
                   int64_t line) {
    SgChan* c = sg_chan_of(h, fn, file, line, "chan_recv");
    sg_mutex_lock(&c->m);
    while (c->n == 0 && !c->closed)
        sg_chan_wait(c);
    if (c->n == 0) {
        sg_mutex_unlock(&c->m);
        sg_panic(fn, file, line, "chan_recv() 通道已关闭且无消息");
    }
    char* msg = sg_chan_pop(c); /* 所有权移交接收方 */
    sg_mutex_unlock(&c->m);
    return msg;
}

void sg_chan_close(int64_t h, const char* fn, const char* file,
                   int64_t line) {
    SgChan* c = sg_chan_of(h, fn, file, line, "chan_close");
    sg_mutex_lock(&c->m);
    if (c->closed) {
        sg_mutex_unlock(&c->m);
        sg_panic(fn, file, line, "chan_close() 通道已关闭");
    }
    c->closed = 1;
    sg_chan_notify_all(c);
    sg_mutex_unlock(&c->m);
}
)TT";
