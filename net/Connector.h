#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 客户端连接对象，连接服务器socket
*/

#include "ConnectionBase.h"
#include "../Util/SpinLockQueue.h"

namespace bllsll {

class EventLoop;

class Connector : public ConnectionBase
{
public:
    Connector(EventLoop* loop, int port, const std::string& strIp);
    ~Connector();

    //收到消息
    virtual void OnRecv(RecvCallback&& );
    //发送消息
    virtual void Send(const char* pData, int nLen);
    //客户端连接成功回调
    virtual void OnConnected(ConnCallback&& callback);
    //读事件处理
    virtual void HandleRead(int fd, uint32_t events);
    //写事件处理
    virtual void HandleWrite(int fd, uint32_t events);
    //断开连接
    virtual void OnDisconnected(DisConnCallback&& callback);
    //accept事件处理
    virtual void HandleAccept(int listenFd, uint32_t events) {}
    //getfd
    virtual int GetFd(){ return socket_; }
protected:
    void _Connect(int port, const std::string& strIp);

private:
    int port_;
    std::string strIp_;
    ConnCallback connCallback_;
};

} //namespace bllsll