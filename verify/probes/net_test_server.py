# 测试服务器：TCP echo(23456) + 固定响应 HTTP(23458) + 立即关闭(23460)
#             + chunked 编码 HTTP(23462)
import socket, threading
def echo():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 23456)); srv.listen(8)
    while True:
        c, _ = srv.accept()
        def handle(c):
            try:
                while True:
                    d = c.recv(4096)
                    if not d: break
                    c.sendall(d)
            except OSError: pass
            finally: c.close()
        threading.Thread(target=handle, args=(c,), daemon=True).start()
def http():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 23458)); srv.listen(8)
    RESP = b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello"
    while True:
        c, _ = srv.accept()
        try:
            c.recv(65536); c.sendall(RESP)
        except OSError: pass
        finally: c.close()
def closer():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 23460)); srv.listen(8)
    while True:
        c, _ = srv.accept()
        c.close()
def httpchunked():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 23462)); srv.listen(8)
    # chunked 编码（hex size 行带 ';ext' 扩展，末块 0 收尾）：
    # "world!"(6) + "12345678"(8) = "world!12345678"
    RESP = (b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n"
            b"6;ext=1\r\nworld!\r\n8\r\n12345678\r\n0\r\n\r\n")
    while True:
        c, _ = srv.accept()
        try:
            c.recv(65536); c.sendall(RESP)
        except OSError: pass
        finally: c.close()
threading.Thread(target=echo, daemon=True).start()
threading.Thread(target=closer, daemon=True).start()
threading.Thread(target=httpchunked, daemon=True).start()
http()
