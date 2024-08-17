#pragma once
/**
@auther: chencaiyu
@date: 2024.8.3
@brief: epoll类封装
*/

#include <vector>
#include "Poller.h"

namespace sll {

class Epollor : public Poller
{
public:
    Epollor(int maxEvents = 1024);
    ~Epollor();
    
    //添加io事件
    virtual void AddEvent(int fd, uint32_t events);
    //修改io事件
    virtual void ModifyEvent(int fd, uint32_t events);
    //删除io事件
    virtual void RemoveEvent(int fd);
    //epoll_wait
    virtual int Wait(int timeout = -1);
    //返回epoll_wait触发事件
    virtual const std::vector<epoll_event>& GetFiredEvents() {
        return events_;
    }

private:
    int epollFd_;
    int maxEvents_;
    std::vector<epoll_event> events_;
};

} //namespace sll