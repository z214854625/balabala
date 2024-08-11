#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 客户端connection对象，连接服务器socket
*/

#include "precompiled.h"
#include "IConnection.h"

namespace sll {

class EventLoop;

class Connector : public IConnection
{
public:
    Connector(EventLoop* loop, int port, const std::string& strIP);
    ~Connector();

    //收到消息
    virtual void OnRecv(RecvCallback&& );
    //发送消息
    virtual void Send(const char* pData, int nLen);
    //客户端连接成功回调
    virtual void OnConnected(ConnCallback&& callback);

private:
    int socket_;
    //sockaddr_in address_;
    std::queue<std::string> sendMQ_;
    int state_;
    EventLoop* loop_;
    RecvCallback recvCallback_;
    ConnCallback connCallback_;
};

} //namespace sll