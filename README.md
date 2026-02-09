# TCP server 

## 0.0

单线程，阻塞IO

## 1.0
基于 C++17 实现单线程事件驱动 TCP Server，采用 Reactor 模型与 epoll 边缘触发 (ET) 处理多客户端连接