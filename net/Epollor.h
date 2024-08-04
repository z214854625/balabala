#pragma once
/**
@auther: chencaiyu
@date: 2024.8.3
@brief: epoll类封装
*/

#include "stdref.h"
#include <sys/epoll.h>

namespace sll {

class Epollor : public Poller
{
public:
    Epollor(int maxEvents = 1024);
    ~Epollor();
    
    //添加io事件
    void AddEvent(int fd, uint32_t events);
    //修改io事件
    void ModifyEvent(int fd, uint32_t events);
    //删除io事件
    void RemoveEvent(int fd);
    //epoll_wait
    int Wait(int timeout = -1);
    //返回epoll_wait触发事件
    const std::vector<epoll_event>& GetFiredEvents() {
        return events_;
    }

private:
    int epollFd_;
    int maxEvents_;
    std::vector<epoll_event> events_;
};

} //namespace sll