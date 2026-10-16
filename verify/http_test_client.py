# http_test_client.py - probe_http_acc_server 的固定请求序列客户端。
# 用法: python http_test_client.py
# 3 个请求（Connection: close 短连接）: GET /、GET /（带 header）、POST /echo。
# 断言响应字节与协议形态（状态行/Content-Length/Connection: close）。
import socket
import sys

HOST, PORT = "127.0.0.1", 23580


def req(raw: str) -> bytes:
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(raw.encode())
    out = b""
    while True:
        b = s.recv(4096)
        if not b:
            break
        out += b
    s.close()
    return out


def check(name: str, cond: bool) -> None:
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        sys.exit(1)


r1 = req("GET / HTTP/1.1\r\nHost: x\r\n\r\n")
check("r1 status", r1.startswith(b"HTTP/1.1 200 OK\r\n"))
check("r1 body", r1.endswith(b"hello-http"))
check("r1 ctype", b"Content-Type: text/plain; charset=utf-8\r\n" in r1)
check("r1 close", b"Connection: close\r\n" in r1)
check("r1 clen", b"Content-Length: 10\r\n" in r1)

r2 = req("GET /missing HTTP/1.1\r\nHost: x\r\nUser-Agent: probe\r\n\r\n")
check("r2 404", r2.startswith(b"HTTP/1.1 404 Not Found\r\n"))
check("r2 body", r2.endswith(b"nope"))

r3 = req("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\n"
         "Content-Length: 6\r\n\r\n你好")
check("r3 echo", r3.endswith("你好".encode()))
check("r3 clen", b"Content-Length: 6\r\n" in r3)  # UTF-8 6 字节

print("CLIENT ALL PASS")
