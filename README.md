# A C++ Lightweight Web Server
Linux下基于模拟Proactor的C ++轻量级Web服务器，目前仅支持GET方法处理静态资源，通过Webbench压力测试可以实现上万的并发连接数据交换

## 核心功能及技术
* 使用 **线程池 + 非阻塞socket + IO多路复用epoll(水平触发LT实现) + 事件处理(模拟Proactor实现)** 的并发模型；
* 利用 **状态机** 解析HTTP请求报文，目前仅支持 **HTTP GET** 方法，实现对静态资源的请求；
* 使用 **线程池** 提高并发度，并降低频繁创建销毁线程的开销；
* 使用 **互斥锁（pthrea_mutex_t）、信号量（sem_t）** 等线程同步互斥机制；
* epoll使用 **EPOLLONESHOT** 保证一个socket连接在任意时刻都只被一个线程处理；
* 经Webbench压力测试可以实现 **上万的并发连接** 数据交换。

## 开发环境
* 操作系统: Ubuntu 18.04.5
* 编译器：g++ 7.5.0
* 版本控制：git
* 集成开发工具：Clion
* 编辑器：Vim
* 压测工具：WebBench

## 实现框架

## 压力测试
![image-webbench](https://github.com/cmyDS/WebServer/blob/main/test_presure/webbench_test10000.png)
```bash
./test_presure/webbench-1.5/webbench -c 10000 -t 5 http://ip:port/index.html
```
* 并发连接总数：10000
* 访问服务器时间：5s
* 所有访问均成功
* QPS 10000+

## 待开发计划
* 比较ET和LT边缘触发实现的epoll性能
* 比较Reactor模式和模拟Proactor模式的高并发模型性能
* 支持HTTP长/短连接
* 支持解析HTTP POST请求
* 定时器处理非活动连接(非活跃连接占用了连接资源，影响服务器性能)
>* 基于小根堆实现定时器，关闭超时的非活动连接
* 添加同步/异步日志系统，记录服务器运行状态
>* 利用单例模式与阻塞队列实现异步的日志系统
* 实现循环缓冲区
* 实现web端用户注册、登录功能
* 利用RAII机制实现了数据库连接池，减少数据库连接建立与关闭的开销

