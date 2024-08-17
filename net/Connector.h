#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 客户端connection对象，连接服务器socket
*/

#include "IConnection.h"
#include "../Util/SpinLockQueue.h"

namespace sll {

class EventLoop;

class Connector : public IConnection
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

protected:
    void _Connect(int port, const std::string& strIp);

private:
    int socket_;
    sll::SpinLockQueue<std::string> sendMQ_;
    int state_;
    EventLoop* loop_;
    RecvCallback recvCallback_;
    ConnCallback connCallback_;
    DisConnCallback disConnCallback_;
    int port_;
    std::string strIp_;
};

} //namespace sll