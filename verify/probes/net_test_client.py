# 服务端探针的客户端：连接 23462，发一行，收回显并打印
import socket, sys
c = socket.create_connection(("127.0.0.1", 23462), timeout=10)
c.sendall(b"srv-echo")
c.shutdown(socket.SHUT_WR)
data = c.recv(64)
print(data.decode())
c.close()
