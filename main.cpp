#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "sql_connection_pool.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main()
{
    const char *ip = "192.168.206.129";
    int port = atoi("9990");

    /*预先为每个可能的客户连接分配一个http_conn对象*/
    http_conn *users = new http_conn[MAX_FD];
    assert(users);
    /*设置数据库连接*/
    //需要修改的数据库信息,登录名,密码,库名
    string User = "root";
    string Passwd = "lw123654m";
    string Databasename = "tinydb";

    /*GetInstance 返回的是一个connection_pool 静态变量 static connection_pool connPool; return &connPool;*/
    connection_pool *connPool = connection_pool::GetInstance();
    /*connPool 初始化了 N个 与数据库的连接*/
    connPool -> init("localhost", User, Passwd, Databasename, 3306, 8);
    //初始化数据库读取表
    users->initmysql_result(connPool);

    /*忽略SIGPIPE信号*/
    /*可以通过设置信号处理函数来忽略 SIGPIPE 信号，使得进程在收到该信号时不做任何处理。
    这样的话，当写入一个已经关闭的套接字时，系统会默默地丢弃写操作，而不是导致进程被中断。*/
    addsig(SIGPIPE, SIG_IGN);
    /*创建线程池
    线程池中的每个线程从被创建之初就开始运行work函数，里面是运行run函数，
    不断监听请求队列，后续一旦有请求到达就开始处理
    */
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    /*
    SO_LINGER选项用于指定套接字关闭时的行为。
    在这里，通过创建一个 struct linger 结构体变量 tmp，
    并将其成员 l_onoff 设置为 1，l_linger 设置为 0。
    这表示在关闭套接字时将使用延迟关闭（linger）选项，并且不等待未发送的数据。

    l_onoff 表示是否启用 SO_LINGER 选项。当设置为非零值时，表示启用 SO_LINGER 选项。
    l_linger 表示在关闭套接字时的延迟时间，单位是秒。
    当设置为 0 时，表示不等待未发送的数据直接关闭套接字；
    当设置为非零值时，表示等待指定的时间（l_linger 秒）后再关闭套接字。
    */
    struct linger tmp = {1, 0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    /*这是主线程的epoll函数，监听listen socket 发过来的连接请求，以及连接建立好后的所有事件请求*/
    /*半同步/半反应堆模式，就是这么干的*/
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    /*listensocket不可以设置oneshot，
    否则应用程序只能处理一个客户端连接，
    因为后续的客户连接请求将不再触发listenfd上的EPOLLIN事件*/
    addfd(epollfd, listenfd, false); 
    /*http_conn连接类的静态变量m_epollfd*/
    http_conn::m_epollfd = epollfd;

    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }
                /*初始化这个连接。将connfd加入到m_epollfd中；初始化读写缓冲区，分配给这个connfd的*/
                printf("Got connection from ip: %s , port: %d\n",inet_ntoa(client_address.sin_addr), ntohs(client_address.sin_port));
                users[connfd].init(connfd, client_address, User, Passwd, Databasename);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                /*如果有异常，直接关闭客户连接*/
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN) /*有数据需要读*/
            {
                /*根据读的结果，决定是将任务添加到线程池，还是关闭连接
                将数据读入到了users[sockfd]的m_read_buf中
                随后将这个任务 httpconn* request = users + sockfd 添加到工作队列中
                依旧是主线程负责将数据读入到对应user[sockfd]的buff中，再交给子线程处理
                子线程就是把buff中的数据（http请求）读出来进行处理，然后再返回相应的资源文件
                */
                if (users[sockfd].read())
                {
                    /*users是一个指针+ sockfd偏移量，就是将users[sockfd]加入到线程池任务中*/
                    pool->append(users + sockfd);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            /*是否可以进行写操作
            当文件描述符上的输出缓冲区变为可写时，会触发 EPOLLOUT 事件*/
            else if (events[i].events & EPOLLOUT)
            {
                /*根据写的结果，决定是否关闭连接*/
                if (!users[sockfd].write())  /*如果不保持连接，就关闭连接，否则维持连接*/
                {
                    users[sockfd].close_conn();
                }
            }
            else
            {}
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}
