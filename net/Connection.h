#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 服务端收到客户端连接的对象，处理epoll回调事件
*/

#include "ConnectionBase.h"
#include "../Util/SpinLockQueue.h"

namespace sll {

class EventLoop;

class Connection : public ConnectionBase
{
public:
    Connection(int fd, EventLoop* loop);
    ~Connection();

    //收到消息
    virtual void OnRecv(RecvCallback&& );
    //发送消息
    virtual void Send(const char* pData, int nLen);
    //客户端连接成功回调
    virtual void OnConnected(ConnCallback&& callback) {}
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //断开连接
    virtual void OnDisconnected(DisConnCallback&& callback);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd() { return socket_; }

private:
};

} //namespace sll