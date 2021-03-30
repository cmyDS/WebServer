# A C++ Lightweight Web Server
Linux下基于模拟Proactor的C ++轻量级Web服务器，目前仅支持GET方法处理静态资源，通过Webbench压力测试可以实现上万的并发连接数据交换。
*使用 线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现) 的并发模型
