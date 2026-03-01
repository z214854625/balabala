#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象基类，处理epoll读写事件（Actor模型版本）
       读到数据后通过ActorSystem路由给绑定的Actor
*/

#include "IConnection.h"
#include "../Util/SpinLockQueue.h"

namespace bllsll {

class EventLoop;

class ConnectionBase : public IConnection
{
public:
    ConnectionBase(EventLoop* loop);
    ~ConnectionBase();

    //发送消息
    virtual void Send(const char* pData, int nLen);
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd() { return 0; }

protected:
    int socket_;
    int state_;
    bllsll::SpinLockQueue<std::string> sendMQ_;
    std::string lastMsgCache_;
    EventLoop* loop_;
};

} //namespace bllsll
