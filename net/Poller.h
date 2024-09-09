#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: Poller基类
*/

#include <vector>
#include <sys/epoll.h>

// LT模式：epoll默认的工作模式，数据没处理完每次都会收到提醒，ET模式：epoll只会对事件提醒一次
#define EPOLL_EVENTS_RW (EPOLLET | EPOLLIN | EPOLLOUT)
#define EPOLL_EVENTS_R (EPOLLET | EPOLLIN)
#define EPOLL_EVENTS_W (EPOLLET | EPOLLOUT)

#define NET_BUFF_SIZE 64*1024

namespace bllsll {

class Poller
{
public:
    virtual ~Poller(){}
    //添加io事件
    virtual void AddEvent(int fd, uint32_t events) = 0;
    //修改io事件
    virtual void ModifyEvent(int fd, uint32_t events) = 0;
    //删除io事件
    virtual void RemoveEvent(int fd) = 0;
    //epoll_wait
    virtual int Wait(int timeout = -1) = 0;
    //返回epoll_wait触发事件
    virtual const std::vector<epoll_event>& GetFiredEvents() = 0;
};

} // namespace bllsll