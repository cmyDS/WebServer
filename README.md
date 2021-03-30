# A C++ Lightweight Web Server
Linux下基于模拟Proactor的C ++轻量级Web服务器，目前仅支持GET方法处理静态资源，通过Webbench压力测试可以实现上万的并发连接数据交换

## 功能
----------
> * 使用 线程池 + 非阻塞socket + epoll(ET和LT均实现) + 事件处理(Reactor和模拟Proactor均实现) 的并发模型
> * 利用正则与状态机解析HTTP请求报文，实现处理静态资源的请求；
> * 利用标准库容器封装char，实现自动增长的缓冲区；
> * 基于小根堆实现的定时器，关闭超时的非活动连接；
> * 利用单例模式与阻塞队列实现异步的日志系统，记录服务器运行状态；
> * 利用RAII机制实现了数据库连接池，减少数据库连接建立与关闭的开销，同时实现了用户注册登录功能。

## 开发环境
> * 操作系统: Ubuntu 18.04.5
> * 编译器：g++ 7.5.0
> * 版本控制：git
> * 集成开发工具：Clion
> * 编辑器：Vim
> * 压测工具：WebBench

## 压力测试
![image-webbench](https://github.com/markparticle/WebServer/blob/master/readme.assest/%E5%8E%8B%E5%8A%9B%E6%B5%8B%E8%AF%95.png)
```bash
./webbench-1.5/webbench -c 100 -t 10 http://ip:port/
./webbench-1.5/webbench -c 1000 -t 10 http://ip:port/
./webbench-1.5/webbench -c 5000 -t 10 http://ip:port/
./webbench-1.5/webbench -c 10000 -t 10 http://ip:port/
```
* 测试环境: Ubuntu:19.10 cpu:i5-8400 内存:8G 
* QPS 10000+

## 待开发计划
> * config配置
> * 完善单元测试
> * 实现循环缓冲区

