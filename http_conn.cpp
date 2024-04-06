#include "http_conn.h"
#include <mysql/mysql.h>

#include <fstream>


/*定义http响应的一些状态信息*/
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

/*网站的根目录*/
const char *doc_root = "docs";
map<string, string> users;
locker m_lock;

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  /*初始化先把EPOLLIN加进来，EPOLLOUT不加进来*/
    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        // modfd( m_epollfd, m_sockfd, EPOLLIN );
        /*关闭这个连接，从epoll中移除这个连接的监听*/
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--; /*关闭连接，客户总量-1*/
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    int error = 0;

    /*下面这四行都是调试用的*/
    socklen_t len = sizeof(error);
    getsockopt(m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* 把当前socket连接加入到epoll内核事件表中*/
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

void http_conn::init()
{
    mysql = NULL;
    cgi = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    /*判断这次请求完成之后，是否关闭这个连接，或者继续保持连接*/
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/*循环读取客户数据，直到无数据可读或者对方关闭连接
read 与 write函数都是交给主线程执行的*/
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;
    while (true)
    {
        /*recv函数参数：sockfd, 读到哪里，读多少字节，*/
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) // 非阻塞模式
            {
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

/*写HTTP响应*/
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;
    int bytes_to_send = m_write_idx;  /*要送出这么多数据*/
    /*没有需要写入m_sockfd的数据，说明没有需求？所以改成侦听EPOLLIN，客户的需求？*/
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {   
        /*向m_sockfd写入数据，从m_iv中，数量是m_iv_count*/
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1)
        {
            /*如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无
            法立即接收到同一客户的下一个请求，但这可以保证连接的完整性*/
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send -= temp;  /*还要送出这么多数据*/
        bytes_have_send += temp;  /*已经送出了这么多数据， 两者加起来等于m_write_idx*/
        if (bytes_to_send <= 0)  /*TODO 没看懂这个判断*/
        {
            /*发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接*/
            unmap();
            if (m_linger)  /*如果要保持连接，就初始化（方便后面的读取与输入）*/
            {
                init();
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                modfd(m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)  /*没有读取到完整的http头部请求行，需要继续读取数据*/
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);  /*继续监听请求 因为当前客户的m_buff是一直保存的*/
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    modfd(m_epollfd, m_sockfd, EPOLLOUT);  /*填充好了，等待可以写的通知，就发出去*/
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*如果当前是在处理消息体并且读到了完整的一行
    或者是在读一整行，那就进入while循环
    */
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {
        /* return m_read_buf + m_start_line;
        parse_line()解析一行之后，会更新m_checked_idx到下一行要解析的内容的起始
        get_line()先获取当前的解析的一行内容，遇到'\0'就终止
        再更新m_start_line = m_checked_idx，准备获取下一次解析的内容
        */
        text = get_line();  
        m_start_line = m_checked_idx;  /*m_checked_idx一直指向的是下一个待解析的文本的起始*/
        printf("got 1 http line: %s\n", text);

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);  /*如果是GET，会设置m_method=GET，最后如果一切正常设置当前m_check_state=CHECK_STATE_HEADER*/
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        /* 这是从 parse_header()里面的如果content_length != 0 那么就跳转过来的*/
        case CHECK_STATE_CONTENT:  
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }

    return NO_REQUEST;
}

/*从状态机，解析一行的内容*/
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';  /*把\n \r 都替换了*/
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')  /*这就说明，整个消息头都读完了，那就确定收到了request                                                                    */
    {
        if (m_method == HEAD)
        {
            return GET_REQUEST;
        }
        /*如果整个消息头都读完了，但是content_length！=0 那就处理消息体*/
        if (m_content_length != 0)  
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        return GET_REQUEST;  /*get / post 都会返回 GET_REQUEST*/
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

// 将要请求的数据/url文件放入内存中，之后用writev读取*/
// http_conn::HTTP_CODE http_conn::do_request()
// {
//     strcpy(m_real_file, doc_root);
//     int len = strlen(doc_root);
//     strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);  /*m_url = /simple.html, m_real_file = docs/simple.html*/
//     if (stat(m_real_file, &m_file_stat) < 0) /*stat() 函数用于获取指定文件的状态信息，包括文件大小、访问权限、修改时间等*/
//     {
//         return NO_RESOURCE;
//     }
//     /*检查权限*/
//     if (!(m_file_stat.st_mode & S_IROTH))
//     {
//         return FORBIDDEN_REQUEST;
//     }
//     /*检查文件是否为目录*/
//     if (S_ISDIR(m_file_stat.st_mode))
//     {
//         return BAD_REQUEST;
//     }
//     /*打开文件，并放到共享内存中*/
//     int fd = open(m_real_file, O_RDONLY);
//     m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);  /*映射区域不与其他进程共享*/
//     close(fd);
//     return FILE_REQUEST;
// }
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');
    /*CGI主要是提供了界面的跳转功能*/
    //处理cgi  p / m_url = 2CGISQL.cgi   3CGISQL.cgi  
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);  /*m_url_real = 2GCISQL.cgi*/
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);   /*放到m_real_file中*/
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)  /*m_string保存了 content 消息体中的内容*/
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
            {
                strcpy(m_url, "/welcome_2.html");
            }
                
            else
                strcpy(m_url, "/logError.html");
        }
    }
    /*编写*/
    if (*(p + 1) == '4' && *(p + 2) == 'C')
    {
        //将用户名和内容提取出来
        //user=123&content=123
        char name[100], content[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)  /*m_string保存了 content 消息体中的内容*/
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 9; m_string[i] != '\0'; ++i, ++j)
            content[j] = m_string[i];
        content[j] = '\0';

        char *sql_insert = (char *)malloc(sizeof(char) * 200);
        strcpy(sql_insert, "INSERT INTO info(user, content) VALUES(");
        strcat(sql_insert, "'");
        strcat(sql_insert, name);
        strcat(sql_insert, "', '");
        strcat(sql_insert, content);
        strcat(sql_insert, "')");  
        m_lock.lock();
        int res = mysql_query(mysql, sql_insert);
        m_lock.unlock();
        strcpy(m_url, "/insert_info.html");
    }

    /*这些不是cgi了，是判断其他的*/
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')  /*用123代号，进行判断，再给出真正的url名字*/
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
         strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')  /*查看数据*/
    {
        /*先生成一个html，保存它*/
        /*查询mysql中的所有数据量的大小*/
        char *sql_query = (char *)malloc(sizeof(char) * 200);
        strcpy(sql_query, "SELECT* from info");

        int res = mysql_query(mysql, sql_query);
        std::vector<std::vector<string> >query_results;
        // 获取查询结果
        MYSQL_RES *result = mysql_store_result(mysql);
        // 遍历查询结果并打印
        int num_fields = mysql_num_fields(result);  /*列*/
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {  /*提取每一行的结果*/
            std::vector<string> tmp;
            tmp.push_back(row[0]);
            tmp.push_back(row[1]);
            query_results.push_back(tmp);
        }
        generate_HTML(query_results);  /* name = tables.html*/

        /*读取这个tables.html*/
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/tables.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')  /*插入数据*/
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/insert_info.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);  /*此时m_url保存了要返回的资源界面*/

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
        {
            return false;
        }
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
            {
                return false;
            }
        }
    }
    default:
    {
        return false;
    }
    }

    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

/*munmap函数释放由mmap创建的这段内存空间*/
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

/*NEW databases*/
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        printf("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/*生成html文件*/
void http_conn:: generate_HTML(std::vector<std::vector<string> >&contents)
{
    int n_row = contents.size();
    std::ofstream outputFile("docs/tables.html", std::ios::trunc);

    // 写入 HTML 头部
    outputFile << "<!DOCTYPE html>\n";
    outputFile << "<html>\n";
    outputFile << "<head>\n";
    outputFile << "<title>Table</title>\n";
    outputFile << "</head>\n";
    outputFile << "<body>\n";

    // 写入表格
    outputFile << "<table border=\"1\">\n";
    for (int i = 0; i < n_row; ++i) {
        outputFile << "<tr>\n";
        outputFile << "<td>" << contents[i][0] << "</td>\n";
        outputFile << "<td>" << contents[i][1] << "</td>\n";
        outputFile << "</tr>\n";
    }
    outputFile << "</table>\n";

    // 写入 HTML 尾部
    outputFile << "</body>\n";
    outputFile << "</html>\n";

    outputFile.close();
    std::cout << "HTML file generated successfully: tables.html" << std::endl;
}