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
  enum enConState {
    eUnknown,
    eConnected,
    eDisconnected,
    };
    //Connection(int socket, const sockaddr_in& address);
    Connection(int port, EventLoop* loop);
    ~Connection();

    void HandleRead(int fd, uint32_t events);
    void HandleWrite(int fd, uint32_t events);
    void HandleAccept(int listenFd, uint32_t events);
    void HandleClose();

    static void SetNonBlocking(int fd);

    void Send(onst std::string&);

    void Close();

protected:
    int StartScoket(const string& strIp, int port);

    int SetSockOpt(int fd);

private:
    int socket_;
    //sockaddr_in address_;
    std::string writeBuffer_;
    int state_;
    EventLoop* loop_;
};

} //namespace sll