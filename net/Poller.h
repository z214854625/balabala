#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: Poller基类
*/

namespace sll {

// LT模式：epoll默认的工作模式，数据没处理完每次都会收到提醒，ET模式：epoll只会对事件提醒一次
#define EPOLL_EVENTS_RW (EPOLLET | EPOLLIN | EPOLLOUT)
#define EPOLL_EVENTS_R (EPOLLET | EPOLLIN)
#define EPOLL_EVENTS_W (EPOLLET | EPOLLOUT)

#define BUFF_SIZE 64*1024

class Poller
{
public:
    virtual ~Poller(){}
};

} // namespace sll