// ============================================================================
//  Scourge 编译器 — HTTP 服务端运行时片段（0.9.8）
// ----------------------------------------------------------------------------
//  职责边界：net_http_read_request（读+解析请求 → SgJson）/ net_http_write_
//  response（组包+发送）。纯 C（_rt.c 拼接段），依赖拼接序在前方的
//  kNetRuntimeC（SOCKET/recv/send/sg_net_sockof/sg_net_wsinit）与
//  kJsonRuntimeC（SgJson/sg_json_new/sg_json_obj_put）；panic 消息与
//  VMNet.cpp 逐字一致（双模差分由 verify_http.ps1 探针锁定）。
// ============================================================================
#pragma once

static const std::string kHttpRuntimeC = R"RT(

/* ---- HTTP 服务端（0.9.8）。契约：请求行恰好两空格三段；header 名小写
 * 化；Content-Length 驱动 body 收满（16MB 上限）；EOF 且零字节 → Json
 * null（keep-alive 退出条件），中途断开 → panic；响应恒 Connection: close
 * （阻塞单线程模型，语言层每请求 accept 一连接）。 ---- */

/* NUL 不安全区间内找子串（标准 C 无 memmem，Windows 同样没有）。 */
static const char* sg_http_find(const char* hay, size_t n, const char* nd,
                                size_t nl) {
    if (nl == 0 || n < nl) return NULL;
    for (size_t k = 0; k + nl <= n; ++k)
        if (hay[k] == nd[0] && memcmp(hay + k, nd, nl) == 0) return hay + k;
    return NULL;
}

/* SgJson 字符串形态构造（拷贝入堆，程序生命周期持有）。 */
static SgJson* sg_http_json_str(const char* s, size_t n) {
    SgJson* j = sg_json_new(4);
    j->s = (char*)malloc(n + 1);
    if (!j->s) exit(1);
    memcpy(j->s, s, n);
    j->s[n] = 0;
    return j;
}

/* buf[0..n) 内第 k 行（\r\n 分隔）的 [行起, 行止) 区间；越界返回 0。 */
static int sg_http_line(const char* buf, size_t n, size_t* a, size_t* b) {
    if (*a >= n) return 0;
    const char* e = sg_http_find(buf + *a, n - *a, "\r\n", 2);
    if (!e) return 0;
    *b = (size_t)(e - buf);
    return 1;
}

SgJson* sg_net_http_read_request(int64_t h, const char* fn, const char* file,
                                 int64_t line) {
    sg_net_wsinit();
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_http_read_request");
    const size_t kMaxHdr = 65536, kMaxBody = 16777216;
    /* recv 累积缓冲（realloc 指数增长） */
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) exit(1);
    const char* hdrTail = NULL;
    while (!hdrTail) {
        if (len > kMaxHdr)
            sg_panic(fn, file, line,
                     "net_http_read_request() 请求头超过 65536 字节上限");
        if (len + 4096 > cap) {
            cap *= 2;
            buf = (char*)realloc(buf, cap);
            if (!buf) exit(1);
        }
        int r = recv(s, buf + len, 4096, 0);
        if (r == SOCKET_ERROR) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAETIMEDOUT)
                sg_panic(fn, file, line, "net_http_read_request() 接收超时");
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                sg_panic(fn, file, line, "net_http_read_request() 接收超时");
#endif
            sg_panic(fn, file, line,
                     "net_http_read_request() 接收失败（连接已断开？）");
        }
        if (r == 0) {
            if (len == 0) { /* 对端关闭且零字节：keep-alive 正常退出 */
                free(buf);
                return sg_json_new(0);
            }
            sg_panic(fn, file, line, "net_http_read_request() 连接在请求中途关闭");
        }
        len += (size_t)r;
        hdrTail = sg_http_find(buf, len, "\r\n\r\n", 4);
    }
    size_t hdrEnd = (size_t)(hdrTail - buf);

    /* 请求行：METHOD SP PATH SP VERSION（恰好两个空格） */
    size_t la = 0, lb = 0;
    if (!sg_http_line(buf, hdrEnd, &la, &lb))
        sg_panic(fn, file, line, "net_http_read_request() 请求行格式非法");
    size_t sp1 = la, sp2 = la;
    int spaces = 0;
    for (size_t k = la; k < lb; ++k) {
        if (buf[k] == ' ') {
            if (spaces == 0) { sp1 = k; ++spaces; }
            else if (spaces == 1) { sp2 = k; ++spaces; }
            else { ++spaces; break; }
        }
    }
    if (spaces != 2 || sp2 == sp1 + 1 || sp2 + 1 == lb)
        sg_panic(fn, file, line, "net_http_read_request() 请求行格式非法");

    SgJson* out = sg_json_new(6);
    sg_json_obj_put(out, "method", 6, sg_http_json_str(buf + la, sp1 - la));
    sg_json_obj_put(out, "path", 4,
                    sg_http_json_str(buf + sp1 + 1, sp2 - sp1 - 1));
    sg_json_obj_put(out, "version", 7,
                    sg_http_json_str(buf + sp2 + 1, lb - sp2 - 1));

    /* header 区：逐行 "Name: Value"，名字 ASCII 小写化。终止边界 hdrEnd：
     * 最后一行的行尾 \r\n 恰是 "\r\n\r\n" 的前两字节，落在窗口外——
     * find 失败即最后一行，行止取 hdrEnd（与 VM 侧 buf.find 全域搜索
     * 语义一致，见 VMNet.cpp）。 */
    SgJson* hdrs = sg_json_new(6);
    int64_t contentLen = -1;
    size_t ha = lb + 2;
    while (ha < hdrEnd) {
        const char* e = sg_http_find(buf + ha, hdrEnd - ha, "\r\n", 2);
        size_t hb = e ? (size_t)(e - buf) : hdrEnd;
        const char* colon = NULL;
        for (size_t k = ha; k < hb; ++k)
            if (buf[k] == ':') { colon = buf + k; break; }
        if (!colon || colon == buf + ha)
            sg_panic(fn, file, line, "net_http_read_request() 请求头行格式非法");
        size_t kn = (size_t)(colon - (buf + ha));
        char* name = (char*)malloc(kn + 1);
        if (!name) exit(1);
        for (size_t k = 0; k < kn; ++k) {
            char c = buf[ha + k];
            name[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        name[kn] = 0;
        const char* vb = colon + 1;
        const char* ve = buf + hb;
        while (vb < ve && (*vb == ' ' || *vb == '\t')) ++vb;
        while (ve > vb && (ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        SgJson* hv = sg_http_json_str(vb, (size_t)(ve - vb));
        sg_json_obj_put(hdrs, name, kn, hv);
        if (contentLen < 0 && kn == 14 && memcmp(name, "content-length", 14) == 0)
            contentLen = strtoll(vb, NULL, 10);
        free(name);
        ha = e ? hb + 2 : hdrEnd;
    }
    sg_json_obj_put(out, "headers", 7, hdrs);

    /* body：按 Content-Length 收满 */
    SgJson* bodyJ = sg_json_new(4);
    if (contentLen > 0) {
        if (contentLen > (int64_t)kMaxBody)
            sg_panic(fn, file, line,
                     "net_http_read_request() 请求体超过 16777216 字节上限");
        size_t blen = len - (hdrEnd + 4);
        char* body = (char*)malloc((size_t)contentLen + 1);
        if (!body) exit(1);
        if (blen > (size_t)contentLen) blen = (size_t)contentLen;
        memcpy(body, buf + hdrEnd + 4, blen);
        while ((int64_t)blen < contentLen) {
            int r = recv(s, body + blen,
                         (int)(((size_t)contentLen - blen > 4096) ? 4096
                                                                  : (size_t)contentLen - blen),
                         0);
            if (r == SOCKET_ERROR) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAETIMEDOUT)
                    sg_panic(fn, file, line, "net_http_read_request() 接收超时");
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    sg_panic(fn, file, line, "net_http_read_request() 接收超时");
#endif
                sg_panic(fn, file, line,
                         "net_http_read_request() 接收失败（连接已断开？）");
            }
            if (r == 0)
                sg_panic(fn, file, line, "net_http_read_request() 连接在请求中途关闭");
            blen += (size_t)r;
        }
        body[blen] = 0;
        bodyJ->s = body;
    } else {
        bodyJ->s = (char*)malloc(1);
        if (!bodyJ->s) exit(1);
        bodyJ->s[0] = 0;
    }
    free(buf);
    sg_json_obj_put(out, "body", 4, bodyJ);
    return out;
}

int64_t sg_net_http_write_response(int64_t h, int64_t status,
                                   const char* ctype, const char* body,
                                   const char* fn, const char* file,
                                   int64_t line) {
    sg_net_wsinit();
    SOCKET s = sg_net_sockof(h, fn, file, line, "net_http_write_response");
    if (status < 100 || status > 599)
        sg_panic(fn, file, line, "net_http_write_response() 状态码超出 100..599");
    static const struct { int code; const char* reason; } kReasons[] = {
        {100, "Continue"}, {101, "Switching Protocols"},
        {200, "OK"}, {201, "Created"}, {202, "Accepted"},
        {204, "No Content"}, {301, "Moved Permanently"}, {302, "Found"},
        {304, "Not Modified"}, {400, "Bad Request"},
        {401, "Unauthorized"}, {403, "Forbidden"}, {404, "Not Found"},
        {405, "Method Not Allowed"}, {409, "Conflict"},
        {415, "Unsupported Media Type"}, {429, "Too Many Requests"},
        {500, "Internal Server Error"}, {501, "Not Implemented"},
        {502, "Bad Gateway"}, {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
    };
    const char* reason = "Unknown";
    for (size_t k = 0; k < sizeof(kReasons) / sizeof(kReasons[0]); ++k)
        if (kReasons[k].code == (int)status) { reason = kReasons[k].reason; break; }
    /* One Obvious Way：固定 Connection: close（与 VM 分支同一契约） */
    size_t blen = strlen(body);
    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      (int)status, reason, ctype, blen);
    if (hl <= 0 || (size_t)hl >= sizeof(hdr))
        sg_panic(fn, file, line, "net_http_write_response() 响应头组装失败");
    int64_t sent = 0;
    size_t total = (size_t)hl + blen;
    /* 头与体一次 send 循环发完（避免两阶段部分写语义分歧） */
    char* pkt = (char*)malloc(total);
    if (!pkt) exit(1);
    memcpy(pkt, hdr, (size_t)hl);
    memcpy(pkt + hl, body, blen);
    while ((size_t)sent < total) {
        size_t left = total - (size_t)sent;
        int chunk = left > 0x40000000 ? 0x40000000 : (int)left;
        int r = send(s, pkt + sent, chunk, 0);
        if (r == SOCKET_ERROR)
            sg_panic(fn, file, line, "net_http_write_response() 发送失败（连接已断开？）");
        sent += (int64_t)r;
    }
    free(pkt);
    return sent;
}
)RT";
