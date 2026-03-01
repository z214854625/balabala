#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: tcp accept处理类（Actor模型版本）
       新连接到达时，通过ActorSystem发送Connected消息给listenerActor
*/

#include "IConnection.h"
#include "Poller.h"

namespace bllsll {

class EventLoop;

class Acceptor : public IConnection
{
public:
    Acceptor(int port, EventLoop* loop, uint32_t listenerActorId);
    ~Acceptor();

    //发送消息（Acceptor不需要发送）
    virtual void Send(const char* pData, int nLen) {}
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events) {}
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events) {}
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events);
    //getfd
    virtual int GetFd() { return socket_; }

protected:
    void _Listen(int port);

private:
    int socket_;
    EventLoop* loop_;
    uint32_t listenerActorId_;  // 通知新连接的目标Actor
};

} //namespace bllsll
