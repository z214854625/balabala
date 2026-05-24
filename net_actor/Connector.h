#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 客户端连接对象（Actor模型版本）
       连接成功后通过ActorSystem发送Connected消息给ownerActor
*/

#include "ConnectionBase.h"

namespace bllsll {

class EventLoop;

class Connector : public ConnectionBase
{
public:
    Connector(EventLoop* loop, int port, const std::string& strIp, uint32_t ownerActorId);
    ~Connector();

    //发送消息
    virtual void Send(const char* pData, int nLen);
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd(){ return socket_; }
    // [Send fast-path] 连接尚未建立时不允许直接 write
    virtual bool CanWriteDirectly() const override { return !connecting_; }

protected:
    void _Connect(int port, const std::string& strIp);

private:
    int port_;
    std::string strIp_;
    bool connecting_ = false;
    uint32_t ownerActorId_;  // 通知连接结果的目标Actor
};

} //namespace bllsll
