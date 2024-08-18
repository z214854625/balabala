#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 连接对象基类，处理epoll读写事件
*/

#include "IConnection.h"
#include "../Util/SpinLockQueue.h"

namespace sll {

class EventLoop;

class ConnectionBase : public IConnection
{
public:
    ConnectionBase(EventLoop* loop);
    ~ConnectionBase();

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
    virtual int GetFd() { return 0; }

protected:
    int socket_;
    int state_;
    sll::SpinLockQueue<std::string> sendMQ_;
    std::string lastMsgCache_;
    EventLoop* loop_;
    RecvCallback recvCallback_;
    DisConnCallback disConnCallback_;
};

} //namespace sll