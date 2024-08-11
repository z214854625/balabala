#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: 服务端Connection对象，处理epoll回调事件
*/

#include "precompiled.h"
#include "IConnection.h"
#include "Poller.h"


namespace sll {

class EventLoop;

class Connection : public IConnection
{
public:
    Connection(int port, EventLoop* loop);
    ~Connection();

    //收到消息
    virtual void OnRecv(RecvCallback&& );
    //发送消息
    virtual void Send(const char* pData, int nLen);
    //客户端连接成功回调
    virtual void OnConnected(ConnCallback&& callback) {}

    void HandleRead(int fd, uint32_t events);
    void HandleWrite(int fd, uint32_t events);
    void HandleAccept(int listenFd, uint32_t events);
    void HandleClose();

    static void SetNonBlocking(int fd);

    void Close();

protected:
    int Listen(int port);

    int SetSockOpt(int fd);

private:
    int socket_;
    //sockaddr_in address_;
    std::queue<std::string> sendMQ_;
    int state_;
    EventLoop* loop_;
    RecvCallback recvCallback_;
};

} //namespace sll