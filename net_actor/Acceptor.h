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
class EventLoopThreadPool;

class Acceptor : public IConnection
{
public:
    Acceptor(int port, EventLoop* loop, uint32_t listenerActorId);
    ~Acceptor();

    // [2026.5 多 Reactor] 设置 SubReactor 线程池。
    // 若设置且池非空，新连接 Round-Robin 派发到 sub loop；
    // 否则新连接仍在主 loop 上（保持单 Reactor 行为）
    void SetThreadPool(EventLoopThreadPool* pool) { threadPool_ = pool; }

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
    EventLoop* loop_;                       // 主 loop（Acceptor 自己所在的 loop）
    EventLoopThreadPool* threadPool_ = nullptr;  // [2026.5] SubReactor 池（可选）
    uint32_t listenerActorId_;  // 通知新连接的目标Actor
};

} //namespace bllsll
