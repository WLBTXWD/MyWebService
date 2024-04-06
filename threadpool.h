/*
基于线程池的并发服务器
消息的传递是用工作队列实现
比进程的消息传递机制，从代码实现上看，方便得多

进程的消息传递：socketpair创建管道进行通信 epoll进行消息通知
*/


#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"
#include "sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*参数thread_number是线程池中线程的数量，max_requests是请求队列中最多允
    许的、等待处理的请求的数量*/
    threadpool(connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    /*往请求队列中添加任务*/
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        /*线程池中的线程数*/
    int m_max_requests;         /*请求队列中允许的最大请求数*/
    pthread_t *m_threads;       /*描述线程池的数组，其大小为m_thread_number*/
    std::list<T *> m_workqueue; /*请求队列*/
    locker m_queuelocker;       /*保护请求队列的互斥锁*/
    sem m_queuestat;            /*是否有任务需要处理*/
    bool m_stop;                /*是否结束线程*/

    connection_pool *m_connPool;  //数据库的连接

};

template <typename T>
threadpool<T>::threadpool(connection_pool *connPool, int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false), m_threads(NULL), m_connPool(connPool)
{
    if ((thread_number <= 0) || (max_requests <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }
    /*创建thread_number个线程，并将它们都设置为脱离线程*/
    for (int i = 0; i < thread_number; ++i)
    {
        printf("create the %dth thread\n", i);
        /*线程创建成功，返回0，否则返回一个非零代码*/
        /*将线程的入口函数设置为 worker，并将当前对象的指针 this 作为参数传递给 worker 函数。
        
        C++程序中使用pthread_create函数时，该函数的第3个参数必须指向一个静态函数
        而要在一个静态函数中使用类的动态成员（包括成员函数和成员变量），则只能通过如下两种方式来实现：
        1. 通过类的静态对象来调用。比如单体模式中，静态函数可以通过类的全局唯一实例来访问动态成员函数。
        2. 将类的对象作为参数传递给该静态函数，然后在静态函数中引用这个对象，并调用其动态方法。
        使用的是第2种方式：将线程参数设置为this指针，然后在worker函数中获取该指针并调用其动态方法run。
        */
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        /*在创建每个线程后，通过调用 pthread_detach() 函数将其设置为脱离线程。
        脱离线程意味着线程在退出时将自动释放其资源，而不需要其他线程调用 pthread_join() 来等待其退出。
        如果任何一个线程的创建或脱离失败，都会抛出 std::exception 异常，并释放之前分配的内存。*/
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    /*操作工作队列时一定要加锁，因为它被所有线程共享*/
    m_queuelocker.lock();
    if (m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)  /*arg是threadpool<T>类对象的指针*/
{  /*worker函数在线程创建之初就已经开始工作了，run函数已经执行了*/
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
/*run函数不断地监听工作队列，一有任务，所有线程开始争抢*/
template <typename T>
void threadpool<T>::run()
{
    while (!m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
        {
            continue;
        }
        /* 这是通过connectionRAII的构造函数，
        将 m_connPool -> getInstance() 返回的一个空闲的数据库连接线程，赋给request->mysql MYSQL类型*/
        connectionRAII mysqlcon(&request->mysql, m_connPool);  
        request->process();
    }
}

#endif
