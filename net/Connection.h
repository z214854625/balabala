#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: Connection对象，处理epoll回调事件
*/

#include "stdref.h"
#include "IConnection.h"
#include "Poller.h"

class EventLoop;

namespace sll {

class Connection : public IConnection
{
public:
    Connection(int port, EventLoop* loop);
    ~Connection();

    //收到消息
    virtual void OnRecv(RecvCallback&& );
    //发送消息
    virtual void Send(const char* pData, int nLen);

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