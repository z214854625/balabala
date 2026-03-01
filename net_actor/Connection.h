#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 服务端收到客户端连接的对象（Actor模型版本）
*/

#include "ConnectionBase.h"

namespace bllsll {

class EventLoop;

class Connection : public ConnectionBase
{
public:
    Connection(int fd, EventLoop* loop);
    ~Connection();

    //发送消息
    virtual void Send(const char* pData, int nLen);
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd() { return socket_; }

private:
};

} //namespace bllsll
