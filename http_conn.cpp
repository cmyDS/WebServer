#include "http_conn.h"

// 定义HTTP响应的一些状态信息（状态码）
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录（在这里是我们服务器资源的路径）
const char* doc_root = "/home/cmy/Linux/webserver/resources";

// 设置文件描述符非阻塞
// 对于读而言:阻塞和非阻塞的区别在于没有数据到达的时候是否立刻返回．
int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 往epoll实例中添加需要监听/检测的文件描述符（epoll实例，要添加的文件描述符，是否要检测EPOLLONESHOT事件）
void addfd( int epollfd, int fd, bool one_shot ) {
    // 要检测的文件描述符事件
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;  // 读事件和挂起事件。对方连接断开
    // 在使用 2.6.17 之后版本内核的服务器系统中，对端连接断开触发的 epoll 事件会包含 EPOLLIN | EPOLLRDHUP
    // 有了这个事件，对端断开连接的异常就可以在底层进行处理了（通过事件判断），不用再移交到上层（通过read等的返回值为0判断）
    // (上层尝试在对端已经 close() 的连接上读取请求，只能读到 EOF，会认为发生异常，报告一个错误
    // 之前我们是这样判断断开连接的:int len = read(...)中len==0)
    // 好的服务器既可以支持水平触发也可以支持边沿触发模式
    // 这里为了简单直接用水平触发，后续可以通过再配置一个bool型函数参数，如：默认为false，水平触发，如果True，水平触发
    if(one_shot)
    {
        // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;  // 保证一个socket连接在任一时刻都只被一个线程处理
    }
    // 将要监听的文件描述符及其相关检测信息添加到epoll实例中
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符并关闭连接：删除不再需要检测（对方断开连接）的文件描述符信息并在服务器端关闭对该客户的连接
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改epoll实例中的文件描述符检测信息，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;


// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);  // 关闭连接
        m_sockfd = -1;  // 这个http_conn对象就没有用了
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 初始化新接受的连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用:？？？？？为什么通信套接字也要设置端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    // 添加到epoll实例中
    addfd( m_epollfd, sockfd, true );
    m_user_count++;  // 总客户数+1（当前服务器要招待的客户总数）
    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为检查请求行
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bzero(m_read_buf, READ_BUFFER_SIZE);  // 清空读缓冲区
    bzero(m_write_buf, READ_BUFFER_SIZE);  // 清空读缓冲区
    bzero(m_real_file, FILENAME_LEN);  //
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    // Q:那读缓冲区什么时候清空，为什么每次读不从头开始读，一次读中如果请求报文只读了一半怎么办
    // A:当发送完响应数据后write()函数中会调用init()函数重新初始化
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        // 当前读索引已经大于数组长度：缓冲区已满（等待下一次再读吧）
        return false;
    }
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0 );  // bytes_read为这次读到的字节数

        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {
                // 没有数据（所有数据都读完了）
                break;
            }
            return false;
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 通过\r\n解析出一行，判断依据即为\r\n，同时将'\r''\n'改变为字符串结束符'\0''\0'
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    // 从当前字符位置开始找\r\n，找到了就返回，而不是直到末尾
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];  // 当前字符
        if ( temp == '\r' ) {
            // 判断后面是不是'\n'
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;  // 行数据不完整，读到的数据末尾了都没有\n
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';  // 将'\r' 换成字符串结束符
                m_read_buf[ m_checked_idx++ ] = '\0';  // 将'\n' 换成字符串结束符
                return LINE_OK;  // 得到完整的一行
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            // 判断前面是不是'\r'
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;  // 得到完整的一行
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET /index.html HTTP/1.1
    // 利用正则表达式的话更简单，但这里用传统方式
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    // m_url指向GET后面的空格
    if (! m_url) {
        return BAD_REQUEST;
    }

    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    // 注意：text的内容表面变成了GET\0/index.html HTTP/1.1，实际上text的内容变成了GET，因为字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    // m_version指向html后面的空格
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }

    /**
     *有的请求行中间不是类似/index.html，而是 http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        // 找/第一次出现的位置：
        m_url = strchr( m_url, '/' );  // /index.html
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }

    // 请求行处理完了，改变主状态机状态
    m_check_state = CHECK_STATE_HEADER; // 主状态机检查状态变成检查头
    return NO_REQUEST;  // 只是解析了请求行，还需要继续往下解析（如果读取的数据只有请求行，说明请求不完整，需要继续读取数据）
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {  // 请求头部会有请求体长度，解析时将其赋值给m_content_length
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;   // 请求报文还没解析完
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;

    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    // 实际上，我们应该处理所有可能的头部字段

    return NO_REQUEST;  // 还要继续往下请求报文解析
}

// 这个项目中我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机，解析请求
// 我们的项目比较简单，没有各种状态都判断，但好的服务器应该每种状态都要判断做相应处理
http_conn::HTTP_CODE http_conn::process_read() {
    // 初始状态
    LINE_STATUS line_status = LINE_OK;  // 从状态机
    HTTP_CODE ret = NO_REQUEST;  // HTTP请求处理结果

    char* text = 0;
    // 一行一行的解析：
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
           || ((line_status = parse_line()) == LINE_OK)) {  // while循环内解析出一行数据
        // A && B || C要注意：如果A && B为真，就不会执行C
        // 所以如果当前正在解析请求体：就不用继续用parse_line()解析出一行
        // m_check_state为主状态机当前所处的状态，初始状态为检查请求行

        // parse_line：通过\r\n解析出一行
        // get_line：将parse_line解析出的这一行获取出来，其实是获取该行起始位置指针
        // 获取一行数据
        text = get_line();  // return m_read_buf + m_start_line

        // m_start_line：下一次要解析的行的起始位置
        m_start_line = m_checked_idx;  // m_checked_idx：当前扫描到的字符在读缓冲区中的位置（parse_line下次解析的开始）
        printf( "got 1 http line: %s\n", text );

        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text ); // 解析请求行
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );  // 解析请求头
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    // 获得了一个完整的客户请求（有些http请求没有请求体）
                    return do_request();  // 处理用户请求，（获取用户请求资源），返回处理结果
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    // 获得了一个完整的客户请求
                    return do_request();  // 处理用户请求（获取用户请求资源），返回处理结果
                }
                line_status = LINE_OPEN;  // 行数据尚且不完整
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 处理用户请求（获取用户请求资源）
// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/nowcoder/webserver/resources" 资源文件夹位置
    strcpy( m_real_file, doc_root );  // 将doc_root的值拷贝到m_real_file
    int len = strlen( doc_root );
    // "/home/nowcoder/webserver/resources/index.html" 
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
   
    // 获取m_real_file文件的相关的状态信息给m_file_stat，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;  // 服务器没有这个客户端请求的资源
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) { // S_IROTH 可访问/读权限
        return FORBIDDEN_REQUEST;  // 没有访问权限
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射：mmap将网页数据映射到内存中，返回内存首地址（之后会将内存数据发送给客户端，注意创建了内存映射，在用完这块内存后要释放它）
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );

    return FILE_REQUEST;  // 获取文件成功
}

// 对内存映射区执行munmap操作，释放资源
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

// 写HTTP响应（有两块不同内存——数组（写缓冲区，m_write_idx）：状态行+响应头部；内存映射：响应正文）
// 由于有两块不连续的内存：使用writev()而非write(),writev因为可以将不连续的内存一次性发送出去
// 使用writev()需要将两块内存封装在iovec型数组中：已在process_write函数中封装好
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数

    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );  // 重置监听事件
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            }
        }
    }
}

// 往写缓冲（自己定义的数组m_write_buf）中按照格式写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {  // ...为可变参数
    // 当前写索引大于写缓冲区长度（即写缓冲区满了）
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }

    va_list arg_list;  
    // 通过va_start获取可变参数列表的第一个参数（即format参数右边的可变参数列表的第一个参数）的地址给arg_list
    va_start( arg_list, format );
    
    // 将可变参数（arg_list）格式化（format）输出到一个字符数组（m_write_buf + m_write_idx）
    // 参数2是参数1可接受的最大字节数
    // 执行成功，返回写入到字符数组中的字符个数（不包含终止符），最大不超过参数2；执行失败，返回负值
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;  // 新的写开始位置

    // 释放arg_list指针
    va_end( arg_list );
    return true;
}

// 写HTTP响应报文的状态行
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

// 写HTTP响应报文的响应头部（不完全）
bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}
// 注意：响应正文已经在内存映射中了，无需再写到写缓冲区（数组m_write_buf）

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

// 添加空行
bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

// 添加响应内容类型（不完全），这里只给出了text/html类型
bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    // 根据不用的HTTP请求解析结果作不同的响应
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:  
            // 只有获取资源成功才会有两块不连续内存
            // 内存映射的缓存区+写缓冲区（数组m_write_buf）
            // 将两块不连续内存封装在iovec型数组中
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 由线程处理业务逻辑
    // 解析HTTP请求：使用有限状态机
    HTTP_CODE read_ret = process_read(); // 解析HTTP请求的结果
    // 解析的流程：

    // 如果解析结果是请求不完整，继续获取客户端数据
    if ( read_ret == NO_REQUEST ) {
        modfd( m_epollfd, m_sockfd, EPOLLIN );  
        // 要继续检测该文件描述符的读事件（这个进程也算完成了对该http_conn对象的客户请求读任务，还没读完的任务就交给下一个进程）
        return;
    }

    // 生成响应：把响应数据准备好，以便主线程下次检测到写事件时进行处理（发回给客户端）
    // 根据HTTP请求的解析结果生成响应，不同结果不同响应
    bool write_ret = process_write( read_ret );  
    if ( !write_ret ) {
        // 响应数据没有成功准备，为什么要关闭连接？？
        close_conn();
    }
    // 响应数据准备好后，修改该文件描述符的检测信息：检测写事件
    modfd( m_epollfd, m_sockfd, EPOLLOUT);  // 缓冲区有空闲就会触发写事件
    // 
}