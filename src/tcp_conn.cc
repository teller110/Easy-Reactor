#include "tcp_conn.h"
#include "msg_head.h"
#include "tcp_server.h"
#include "print_error.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <netinet/tcp.h>

static void tcp_rcb(event_loop* loop, int fd, void *args)
{
    tcp_conn* conn = (tcp_conn*)args;
    conn->handle_read();
}

static void tcp_wcb(event_loop* loop, int fd, void *args)
{
    tcp_conn* conn = (tcp_conn*)args;
    conn->handle_write();
}

void tcp_conn::init(int connfd, event_loop* loop)
{
    _connfd = connfd;
    _loop = loop;
    //set NONBLOCK
    int flag = ::fcntl(_connfd, F_GETFL, 0);
    ::fcntl(_connfd, F_SETFL, O_NONBLOCK | flag);

    //set NODELAY
    int opend = 1;
    int ret = ::setsockopt(_connfd, IPPROTO_TCP, TCP_NODELAY, &opend, sizeof(opend));
    error_if(ret < 0, "setsockopt TCP_NODELAY");

    _loop->add_ioev(_connfd, tcp_rcb, EPOLLIN | EPOLLET, this);

    tcp_server::inc_conn();
}

void tcp_conn::handle_read()
{
    int ret = ibuf.read_data(_connfd);
    if (ret == -1)
    {
        //read data error
        error_log("read data from socket");
        clean_conn();
        return ;
    }
    else if (ret == 0)
    {
        //The peer is closed, return -2
        info_log("connection closed by peer");
        clean_conn();
        return ;
    }
    commu_head head;
    while (ibuf.length() >= COMMU_HEAD_LENGTH)
    {
        ::memcpy(&head, ibuf.data(), COMMU_HEAD_LENGTH);
        if (head.length > MSG_LENGTH_LIMIT || head.length < 0)
        {
            //data format is messed up
            error_log("data format error in data head, close connection");
            clean_conn();
            break;
        }
        if (ibuf.length() < COMMU_HEAD_LENGTH + head.length)
        {
            //this is half-package
            break;
        }
        //find in dispatcher
        if (!tcp_server::dispatcher.exist(head.cmdid))
        {
            //data format is messed up
            error_log("this message has no corresponding callback, close connection");
            clean_conn();
            break;
        }
        ibuf.pop(COMMU_HEAD_LENGTH);
        //domain: call user callback
        tcp_server::dispatcher.cb(ibuf.data(), head.length, head.cmdid, this);
        ibuf.pop(head.length);
    }
    ibuf.adjust();
}

void tcp_conn::handle_write()
{
    if (obuf.length())
    {
        int ret = obuf.write_fd(_connfd);
        if (ret == -1)
        {
            error_log("write TCP buffer error, close connection");
            clean_conn();
            return ;
        }
    }
    if (!obuf.length())
    {
        _loop->del_ioev(_connfd, EPOLLOUT);
    }
}

int tcp_conn::send_data(const char* data, int datlen, int cmdid)
{
    bool need_listen = false;
    if (!obuf.length())
        need_listen = true;
    //write rsp head first
    commu_head head;
    head.cmdid = cmdid;
    head.length = datlen;
    //write head
    int ret = obuf.send_data((const char*)&head, COMMU_HEAD_LENGTH);
    if (ret != 0)
        return -1;
    //write content
    ret = obuf.send_data(data, datlen);
    if (ret != 0)
    {
        //只好取消写入的消息头
        obuf.pop(COMMU_HEAD_LENGTH);
        return -1;
    }

    if (need_listen)
    {
        _loop->add_ioev(_connfd, tcp_wcb, EPOLLOUT, this);
    }
    return 0;
}

void tcp_conn::clean_conn()
{
    tcp_server::dec_conn();
    _loop->del_ioev(_connfd);
    _loop = NULL;
    ibuf.clear();
    obuf.clear();
    int fd = _connfd;
    _connfd = -1;
    ::close(fd);
}