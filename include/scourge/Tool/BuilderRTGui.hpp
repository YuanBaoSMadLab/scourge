// ============================================================================
//  Copyright (c) 2026 Scourge Project. All rights reserved.
//  AOT C 运行时片段 — GUI M1+M2：轮询式窗口内建（gui 域，仅 AOT）
// ----------------------------------------------------------------------------
//  职责边界：与 BuilderRTThread.hpp 同模式——build 时拼接在 kThreadRuntimeC
//  之后落盘（复用 kCollRuntimeC 的 SgDict/sg_dict_new 与 kRuntimeC 的
//  sg_panic/sg_dup、kIoRuntimeC 的 sg_io_msg），缓存键已计入本片段文本。
//  事件契约（单一事实源，LangOps.td 注释同步）：gui_poll 返回 Dict[str,str]，
//  恒含 tag/x/y/key/text 五键（"" = 该键无值），空 Dict = 无事件。事件源：
//    * tag="close"  用户点窗口 X（gui_close() 主动关闭不产生事件）；
//    * tag="button" 按钮点击（text=按钮文本，x/y=鼠标屏幕坐标）；
//    * tag="input"  输入框内回车（text=当前输入内容）；
//    * tag="key"    窗口键盘字符（key=该字符 UTF-8 编码）。
//  语义契约：
//    * 句柄 = 1 起始静态表下标，窗口与控件同表（kind 区分）；跨 WM 销毁
//      自清（窗口销毁连带控件槽）；
//    * 单线程模型：事件只在 gui_poll 泵消息时产生（PeekMessage 非阻塞），
//      事件队列满（1024）丢弃最旧；
//    * 文本一律 UTF-8：经 W 系列 API + CP_UTF8 转换（中文标题/文本不乱码）；
//    * POSIX 目标：桩函数运行时 panic（平台绑定内建显式单模，GPU 先例）；
//      Windows 链接 -luser32 -lgdi32 由 Builder 提供。
//  本文件常量字符串含中文（panic 消息对齐 VM 风格），cl /utf-8 由 Builder
//  保证。
// ============================================================================
#pragma once

inline const char kGuiRuntimeC[] = R"TT(/* Scourge AOT 运行时 — GUI 片段（M1+M2；事件契约见本片段头注释）。 */
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SG_GUI_MAX 256
#define SG_GUI_EV_MAX 1024

typedef struct {
    int used, kind;  /* kind：0=窗口 1=label 2=button 3=input */
    int parent;      /* 父窗口句柄（窗口自身为 0）；销毁连带清槽用 */
#ifdef _WIN32
    HWND hwnd;
#endif
} SgGui;

typedef struct {
    char *tag, *x, *y, *key, *text; /* 五键全量快照（sg_dup，不回收） */
} SgGuiEv;

static SgGui sg_gui_tab[SG_GUI_MAX];
static void* sg_gui_defproc[SG_GUI_MAX]; /* input 原窗口过程（子类化） */
static SgGuiEv sg_gui_evq[SG_GUI_EV_MAX];
static int sg_gui_ev_n, sg_gui_ev_head;

static void sg_gui_push(const char* tag, const char* x, const char* y,
                        const char* key, const char* text) {
    if (sg_gui_ev_n >= SG_GUI_EV_MAX) return; /* 满则丢弃（事件非持久承诺） */
    SgGuiEv* e = &sg_gui_evq[(sg_gui_ev_head + sg_gui_ev_n) % SG_GUI_EV_MAX];
    e->tag = sg_dup(tag);
    e->x = sg_dup(x);
    e->y = sg_dup(y);
    e->key = sg_dup(key);
    e->text = sg_dup(text);
    ++sg_gui_ev_n;
}

#ifdef _WIN32
static int64_t sg_gui_i64(int64_t v) { /* 坐标 → 串（小整数，snprintf 恒容纳） */
    char buf[24];
    snprintf(buf, sizeof buf, "%lld", (long long)v);
    return (int64_t)(size_t)sg_dup(buf);
}

/* WM_CHAR 的 UTF-16 单元 → UTF-8 串（代理对不拆，成对字符走两次 key 事件） */
static char* sg_gui_key_utf8(WPARAM wp) {
    uint32_t cp = (uint32_t)wp;
    char buf[8];
    size_t n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    buf[n] = 0;
    return sg_dup(buf);
}

/* HWND → 槽下标（1 起始；未登记 = 0）。 */
static int sg_gui_idx(HWND w) {
    return (int)(intptr_t)GetWindowLongPtrW(w, GWLP_USERDATA);
}

static void sg_gui_free_slot(int idx) {
    if (idx < 1 || idx > SG_GUI_MAX) return;
    sg_gui_tab[idx - 1].used = 0;
    sg_gui_tab[idx - 1].hwnd = NULL;
    sg_gui_defproc[idx - 1] = NULL;
}

static void sg_gui_free_tree(int idx) { /* 窗口销毁连带控件槽 */
    for (int k = 1; k <= SG_GUI_MAX; ++k)
        if (sg_gui_tab[k - 1].used && sg_gui_tab[k - 1].parent == idx)
            sg_gui_free_slot(k);
    sg_gui_free_slot(idx);
}

static LRESULT CALLBACK sg_gui_editproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    int idx = sg_gui_idx(w);
    WNDPROC def = (WNDPROC)sg_gui_defproc[idx - 1];
    if (m == WM_KEYDOWN && wp == VK_RETURN && idx >= 1 && idx <= SG_GUI_MAX &&
        sg_gui_tab[idx - 1].used) {
        int n = GetWindowTextLengthW(w);
        wchar_t* wt = (wchar_t*)malloc((size_t)(n + 1) * sizeof(wchar_t));
        if (wt) {
            GetWindowTextW(w, wt, n + 1);
            int nb = WideCharToMultiByte(CP_UTF8, 0, wt, -1, NULL, 0, NULL,
                                         NULL);
            char* u8 = (char*)malloc((size_t)nb);
            if (u8) {
                WideCharToMultiByte(CP_UTF8, 0, wt, -1, u8, nb, NULL, NULL);
                sg_gui_push("input", "0", "0", "", u8);
                free(u8);
            }
            free(wt);
        }
    }
    return CallWindowProcW(def ? def : DefWindowProcW, w, m, wp, lp);
}

static LRESULT CALLBACK sg_gui_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) { /* 用户点 X：入 close 事件 + 销毁（WM_DESTROY 清槽） */
        sg_gui_push("close", "0", "0", "", "");
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_DESTROY) {
        int idx = sg_gui_idx(w);
        if (idx >= 1 && idx <= SG_GUI_MAX && sg_gui_tab[idx - 1].used &&
            sg_gui_tab[idx - 1].hwnd == w)
            sg_gui_free_tree(idx);
        return 0;
    }
    if (m == WM_COMMAND && HIWORD(wp) == BN_CLICKED) {
        int idx = (int)(int16_t)LOWORD(wp); /* HMENU = 子控件句柄值 */
        if (idx >= 1 && idx <= SG_GUI_MAX && sg_gui_tab[idx - 1].used) {
            HWND bw = sg_gui_tab[idx - 1].hwnd;
            int n = GetWindowTextLengthW(bw);
            wchar_t* wt = (wchar_t*)malloc((size_t)(n + 1) * sizeof(wchar_t));
            char* u8 = NULL;
            if (wt) {
                GetWindowTextW(bw, wt, n + 1);
                int nb = WideCharToMultiByte(CP_UTF8, 0, wt, -1, NULL, 0,
                                             NULL, NULL);
                u8 = (char*)malloc((size_t)nb);
                if (u8)
                    WideCharToMultiByte(CP_UTF8, 0, wt, -1, u8, nb, NULL,
                                        NULL);
                free(wt);
            }
            POINT pt;
            GetCursorPos(&pt);
            char xs[16], ys[16];
            snprintf(xs, sizeof xs, "%ld", (long)pt.x);
            snprintf(ys, sizeof ys, "%ld", (long)pt.y);
            sg_gui_push("button", xs, ys, "", u8 ? u8 : "");
            free(u8);
        }
        return 0;
    }
    if (m == WM_CHAR) {
        char* k = sg_gui_key_utf8(wp);
        sg_gui_push("key", "0", "0", k, "");
        free(k);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* 句柄校验：通用控件句柄 / 窗口句柄。失败 panic（对齐 VM 风格）。 */
static SgGui* sg_gui_of(int64_t h, const char* fn, const char* file,
                        int64_t line, const char* call) {
    if (h < 1 || h > SG_GUI_MAX || !sg_gui_tab[h - 1].used)
        sg_panic(fn, file, line, sg_io_msg(call, "() 无效的句柄"));
    return &sg_gui_tab[h - 1];
}
static SgGui* sg_gui_winof(int64_t h, const char* fn, const char* file,
                           int64_t line, const char* call) {
    SgGui* p = sg_gui_of(h, fn, file, line, call);
    if (p->kind != 0)
        sg_panic(fn, file, line, sg_io_msg(call, "() 需要窗口句柄"));
    return p;
}

static wchar_t* sg_gui_u16(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!w) exit(1);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static HWND sg_gui_create(int idx, const wchar_t* cls, DWORD style,
                          HWND parent, HMENU menu, const wchar_t* text,
                          int x, int y, int w, int h) {
    HWND hc = CreateWindowExW(0, cls, text, style, x, y, w, h, parent, menu,
                              NULL, NULL);
    if (!hc) return NULL;
    SetWindowLongPtrW(hc, GWLP_USERDATA, (LONG_PTR)idx);
    SendMessageW(hc, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
                 TRUE);
    return hc;
}

int64_t sg_gui_window(const char* title, int64_t w, int64_t h,
                      const char* fn, const char* file, int64_t line) {
    static int class_done;
    if (!class_done) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = sg_gui_wndproc;
        wc.lpszClassName = L"ScourgeGui";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        if (!RegisterClassW(&wc))
            sg_panic(fn, file, line, "gui_window() 窗口类注册失败");
        class_done = 1;
    }
    int idx = 0;
    for (int k = 1; k <= SG_GUI_MAX; ++k)
        if (!sg_gui_tab[k - 1].used) { idx = k; break; }
    if (!idx)
        sg_panic(fn, file, line,
                 sg_io_msg("gui_window", "() 控件表已满（上限 256）"));
    wchar_t* wt = sg_gui_u16(title);
    HWND hw = CreateWindowExW(0, L"ScourgeGui", wt, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, (int)w, (int)h,
                              NULL, NULL, NULL, NULL);
    free(wt);
    if (!hw)
        sg_panic(fn, file, line, sg_io_msg("gui_window", "() 窗口创建失败"));
    sg_gui_tab[idx - 1].used = 1;
    sg_gui_tab[idx - 1].kind = 0;
    sg_gui_tab[idx - 1].parent = 0;
    sg_gui_tab[idx - 1].hwnd = hw;
    SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)idx);
    ShowWindow(hw, SW_SHOW);
    return idx;
}

static int64_t sg_gui_child(int64_t win, const char* fn, const char* file,
                            int64_t line, const char* call) {
    SgGui* p = sg_gui_winof(win, fn, file, line, call);
    int idx = 0;
    for (int k = 1; k <= SG_GUI_MAX; ++k)
        if (!sg_gui_tab[k - 1].used) { idx = k; break; }
    if (!idx)
        sg_panic(fn, file, line,
                 sg_io_msg(call, "() 控件表已满（上限 256）"));
    sg_gui_tab[idx - 1].used = 1;
    sg_gui_tab[idx - 1].parent = (int)win;
    (void)p;
    return idx;
}

int64_t sg_gui_label(int64_t win, const char* text, int64_t x, int64_t y,
                     int64_t w, int64_t h, const char* fn, const char* file,
                     int64_t line) {
    int64_t idx = sg_gui_child(win, fn, file, line, "gui_label");
    wchar_t* wt = sg_gui_u16(text);
    HWND hc = sg_gui_create((int)idx, L"STATIC",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            sg_gui_tab[win - 1].hwnd, NULL, wt, (int)x,
                            (int)y, (int)w, (int)h);
    free(wt);
    if (!hc)
        sg_panic(fn, file, line, sg_io_msg("gui_label", "() 控件创建失败"));
    sg_gui_tab[idx - 1].kind = 1;
    sg_gui_tab[idx - 1].hwnd = hc;
    return idx;
}

int64_t sg_gui_button(int64_t win, const char* text, int64_t x, int64_t y,
                      int64_t w, int64_t h, const char* fn, const char* file,
                      int64_t line) {
    int64_t idx = sg_gui_child(win, fn, file, line, "gui_button");
    wchar_t* wt = sg_gui_u16(text);
    HWND hc = sg_gui_create((int)idx, L"BUTTON",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            sg_gui_tab[win - 1].hwnd, (HMENU)(intptr_t)idx,
                            wt, (int)x, (int)y, (int)w, (int)h);
    free(wt);
    if (!hc)
        sg_panic(fn, file, line, sg_io_msg("gui_button", "() 控件创建失败"));
    sg_gui_tab[idx - 1].kind = 2;
    sg_gui_tab[idx - 1].hwnd = hc;
    return idx;
}

int64_t sg_gui_input(int64_t win, int64_t x, int64_t y, int64_t w, int64_t h,
                     const char* fn, const char* file, int64_t line) {
    int64_t idx = sg_gui_child(win, fn, file, line, "gui_input");
    HWND hc = sg_gui_create((int)idx, L"EDIT",
                            WS_CHILD | WS_VISIBLE | WS_BORDER |
                                ES_AUTOHSCROLL,
                            sg_gui_tab[win - 1].hwnd, NULL, L"",
                            (int)x, (int)y, (int)w, (int)h);
    if (!hc)
        sg_panic(fn, file, line, sg_io_msg("gui_input", "() 控件创建失败"));
    sg_gui_tab[idx - 1].kind = 3;
    sg_gui_tab[idx - 1].hwnd = hc;
    sg_gui_defproc[idx - 1] = (void*)SetWindowLongPtrW(
        hc, GWLP_WNDPROC, (LONG_PTR)sg_gui_editproc);
    return idx;
}

char* sg_gui_get_text(int64_t h, const char* fn, const char* file,
                      int64_t line) {
    SgGui* p = sg_gui_of(h, fn, file, line, "gui_get_text");
    int n = GetWindowTextLengthW(p->hwnd);
    wchar_t* wt = (wchar_t*)malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!wt) exit(1);
    GetWindowTextW(p->hwnd, wt, n + 1);
    int nb = WideCharToMultiByte(CP_UTF8, 0, wt, -1, NULL, 0, NULL, NULL);
    char* u8 = (char*)malloc((size_t)nb);
    if (!u8) exit(1);
    WideCharToMultiByte(CP_UTF8, 0, wt, -1, u8, nb, NULL, NULL);
    free(wt);
    return u8;
}

void sg_gui_set_text(int64_t h, const char* text, const char* fn,
                     const char* file, int64_t line) {
    SgGui* p = sg_gui_of(h, fn, file, line, "gui_set_text");
    wchar_t* wt = sg_gui_u16(text);
    SetWindowTextW(p->hwnd, wt);
    free(wt);
}

void sg_gui_title(int64_t win, const char* title, const char* fn,
                  const char* file, int64_t line) {
    SgGui* p = sg_gui_winof(win, fn, file, line, "gui_title");
    wchar_t* wt = sg_gui_u16(title);
    SetWindowTextW(p->hwnd, wt);
    free(wt);
}

void sg_gui_show(int64_t h, const char* fn, const char* file, int64_t line) {
    ShowWindow(sg_gui_of(h, fn, file, line, "gui_show")->hwnd, SW_SHOW);
}

void sg_gui_hide(int64_t h, const char* fn, const char* file, int64_t line) {
    ShowWindow(sg_gui_of(h, fn, file, line, "gui_hide")->hwnd, SW_HIDE);
}

void sg_gui_close(int64_t win, const char* fn, const char* file,
                  int64_t line) {
    /* 主动关闭不产生 close 事件；槽由 WM_DESTROY 清（连带控件） */
    SgGui* p = sg_gui_winof(win, fn, file, line, "gui_close");
    DestroyWindow(p->hwnd);
}

SgDict* sg_gui_poll(int64_t win, const char* fn, const char* file,
                    int64_t line) {
    sg_gui_winof(win, fn, file, line, "gui_poll");
    MSG msg;
    while (sg_gui_ev_n == 0 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (sg_gui_ev_n == 0)
        return sg_dict_new(2, 2, 0, NULL, NULL, fn, file, line);
    SgGuiEv e = sg_gui_evq[sg_gui_ev_head];
    sg_gui_ev_head = (sg_gui_ev_head + 1) % SG_GUI_EV_MAX;
    --sg_gui_ev_n;
    static const char* keys[5] = {"tag", "x", "y", "key", "text"};
    char* vals[5] = {e.tag, e.x, e.y, e.key, e.text};
    int64_t ks[5], vs[5];
    for (int k = 0; k < 5; ++k) {
        ks[k] = (int64_t)(size_t)keys[k];
        vs[k] = (int64_t)(size_t)vals[k];
    }
    /* 事件串所有权移交 Dict 槽（运行时 arena 纪律：不回收） */
    return sg_dict_new(2, 2, 5, ks, vs, fn, file, line);
}

#else /* POSIX 目标：桩（平台绑定内建显式单模） */

static void sg_gui_stub(const char* fn, const char* file, int64_t line) {
    sg_panic(fn, file, line, "gui_* 仅支持 Windows 目标（Win32 AOT 专属）");
}
int64_t sg_gui_window(const char* t, int64_t w, int64_t h, const char* fn,
                      const char* file, int64_t line) {
    (void)t; (void)w; (void)h;
    sg_gui_stub(fn, file, line);
    return 0;
}
int64_t sg_gui_label(int64_t a, const char* t, int64_t x, int64_t y,
                     int64_t w, int64_t h, const char* fn, const char* file,
                     int64_t line) {
    (void)a; (void)t; (void)x; (void)y; (void)w; (void)h;
    sg_gui_stub(fn, file, line);
    return 0;
}
int64_t sg_gui_button(int64_t a, const char* t, int64_t x, int64_t y,
                      int64_t w, int64_t h, const char* fn, const char* file,
                      int64_t line) {
    (void)a; (void)t; (void)x; (void)y; (void)w; (void)h;
    sg_gui_stub(fn, file, line);
    return 0;
}
int64_t sg_gui_input(int64_t a, int64_t x, int64_t y, int64_t w, int64_t h,
                     const char* fn, const char* file, int64_t line) {
    (void)a; (void)x; (void)y; (void)w; (void)h;
    sg_gui_stub(fn, file, line);
    return 0;
}
char* sg_gui_get_text(int64_t a, const char* fn, const char* file,
                      int64_t line) {
    (void)a;
    sg_gui_stub(fn, file, line);
    return NULL;
}
void sg_gui_set_text(int64_t a, const char* t, const char* fn,
                     const char* file, int64_t line) {
    (void)a; (void)t;
    sg_gui_stub(fn, file, line);
}
void sg_gui_title(int64_t a, const char* t, const char* fn, const char* file,
                  int64_t line) {
    (void)a; (void)t;
    sg_gui_stub(fn, file, line);
}
void sg_gui_show(int64_t a, const char* fn, const char* file, int64_t line) {
    (void)a;
    sg_gui_stub(fn, file, line);
}
void sg_gui_hide(int64_t a, const char* fn, const char* file, int64_t line) {
    (void)a;
    sg_gui_stub(fn, file, line);
}
void sg_gui_close(int64_t a, const char* fn, const char* file,
                  int64_t line) {
    (void)a;
    sg_gui_stub(fn, file, line);
}
SgDict* sg_gui_poll(int64_t a, const char* fn, const char* file,
                    int64_t line) {
    (void)a;
    sg_gui_stub(fn, file, line);
    return NULL;
}
#endif
)TT";
