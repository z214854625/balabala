#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "Acceptor.h"
#include "EventLoop.h"
#include "Connection.h"
#include "IOHelper.h"

using namespace bllsll;
using namespace std;

Acceptor::Acceptor(int port, EventLoop* loop) : socket_(-1), loop_(loop)
{
    _Listen(port);
}

Acceptor::~Acceptor()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Acceptor::OnConnected(ConnCallback&& callback)
{
    connCallback_ = std::forward<ConnCallback>(callback);
}

void Acceptor::HandleAccept(int listenFd, uint32_t events)
{
    if ((events & EPOLLIN) == 0) {
        std::cout << "HandleAccept events error." << events << ", fd= " << listenFd << std::endl;
        return;
    }
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    int clientFd = ::accept(listenFd, (sockaddr*)&addr, &addrLen);
    if (clientFd == -1) {
        std::cout << "HandleAccept accept failed! fd= " << listenFd << ", errno=" << errno << std::endl;
        return;
    }
    std::cout << "Accepted new connection. clientFd=" << clientFd << std::endl;
    IOHelper::SetNonBlocking(clientFd);
    IConnection* pNewConn = new Connection(clientFd, loop_);
    if (pNewConn == nullptr) {
        std::cout << "HandleAccept pNewConn null. clientFd=" << clientFd << std::endl;
        return;
    }
    loop_->AddConnection(pNewConn);
    connCallback_(pNewConn);
    loop_->AddEvent(clientFd, EPOLL_EVENTS_RW, [this](int fd, uint32_t event) {
        auto pConn = loop_->GetConnection(fd);
        if (pConn == nullptr) {
            std::cout << "HandleAccept pConn null. fd=" << fd << ", event=" << event << std::endl;
            return;
        }
        if (event & EPOLLIN){
            pConn->HandleRead(fd, event);
        }
        if (event & EPOLLOUT){
            pConn->HandleWrite(fd, event);
        }
    });
}

void Acceptor::_Listen(int port)
{
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == -1) {
        throw std::runtime_error("Acceptor create socket failed! errno=" + to_string(errno));
    }
    IOHelper::SetNonBlocking(socket_);
    IOHelper::SetSockOpt(socket_);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if (bind(socket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        close(socket_);
        throw std::runtime_error("Acceptor bind failed! errno=" + to_string(errno));
    }
    if (listen(socket_, 5) == -1) {
        close(socket_);
        throw std::runtime_error("Acceptor listen failed! errno=" + to_string(errno));
    }
    loop_->AddConnection(this); //添加到连接列表
    loop_->AddEvent(socket_, EPOLL_EVENTS_R, [this](int fd, uint32_t events) {
        auto pAccept = loop_->GetConnection(fd);
        if (pAccept == nullptr) {
            std::cout << "Acceptor::_Listen pAccept null. fd=" << fd << ", event=" << events << std::endl;
            return;
        }
        pAccept->HandleAccept(fd, events);
    });
    std::cout << "listen suc! port= " << port << std::endl;
}
