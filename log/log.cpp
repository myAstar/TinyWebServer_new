#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置。可选择的参数有日志文件、是否关闭日志（默认关闭）、日志缓冲区大小、最大行数以及最长日志条队列
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size（异步队列）,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }
    
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;  //输出内容的长度
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    //file_name是带/的    
    const char *p = strrchr(file_name, '/');    //查找字符串在另一个字符串中最后一次出现的位置，并返回从该位置到字符串结尾的所有字符。
    char log_full_name[256] = {0};

    
    if (p == NULL)  //若输入的文件名没有/，则直接将时间+文件名作为日志名
    {   //拼接日志名字：并将字符串复制到log_full_name中，255为写入字符最大数量
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);    //log_name为自定义文件名，将file_name最后一个‘/’之后的字符串赋给log_name
        strncpy(dir_name, file_name, p - file_name + 1);    //p - file_name + 1是文件所在路径文件夹的长度（指针相减，相当于取最后一个/及前面的路径字符），dirname相当于./
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    
    m_fp = fopen(log_full_name, "a");   //追加形式创建文件
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    
    //日志分级写入
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;  //更新现有行数

     //日志不是今天或写入的日志行数是最大行的倍数
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) //everyday log
    {
        
        //关闭现有日志
        fflush(m_fp);
        fclose(m_fp);

        char new_log[256] = {0};
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday); //拼接时间
       
        if (m_today != my_tm.tm_mday)   //如果不是当前天，那么创建新的日志，更新m_today和m_count
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name); //dir_name+时间+日志名
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else    //如果日志已经达到最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a"); //换成新的日志句柄
    }
 
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);    //将传入的format参数赋值给valst，便于格式化输出

    string log_str;
    m_mutex.lock();

    //写入的具体时间内容格式，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    
    //将格式化的数据从变量参数列表写入大小已设置的缓冲区
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);    //m_buf + n前面是时间部分，
    m_buf[n + m] = '\n';    //行尾加换行符与结束符
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full()) //如果是异步日志，将行数据放到队列
    {
        m_log_queue->push(log_str); //压入一行数据到代写入队列，string形式
    }
    else    //如果是同步日志，将行数据直接写入文件
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);  // void va_start(va_list ap, last_arg) 初始化 ap 变量，它与 va_arg 和 va_end 宏是一起使用的。
}

void Log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
