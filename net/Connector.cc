#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "Connector.h"
#include "Poller.h"
#include "EventLoop.h"
#include "Poller.h"

using namespace sll;
using namespace std;

Connector::Connector(EventLoop* loop, int port, const std::string& strIp) 
    : ConnectionBase(loop), port_(port), strIp_(strIp)
{
}

Connector::~Connector()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Connector::OnRecv(RecvCallback&& callback)
{
    ConnectionBase::OnRecv(std::forward<RecvCallback>(callback));
}

void Connector::Send(const char* pData, int nLen)
{
    ConnectionBase::Send(pData, nLen);
}

void Connector::OnConnected(ConnCallback&& callback)
{
    connCallback_ = std::forward<ConnCallback>(callback);
    _Connect(port_, strIp_);
}

void Connector::HandleRead(int fd, uint32_t events)
{
    ConnectionBase::HandleRead(fd, events);
}

void Connector::HandleWrite(int fd, uint32_t events)
{
    ConnectionBase::HandleWrite(fd, events);
}

void Connector::OnDisconnected(DisConnCallback&& callback)
{
    ConnectionBase::OnDisconnected(std::forward<DisConnCallback>(callback));
}

void Connector::_Connect(int port, const std::string& strIp)
{
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        throw std::runtime_error("Connector create socket failed! errno=" + to_string(errno));
    }

    // 设置为非阻塞模式
    int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);

    std::cout << "connect socket_="<< socket_ << ", port=" << port << ", ip=" << strIp << std::endl;
    //设置ip和端口
    sockaddr_in svrAddr;
    memset(&svrAddr, 0, sizeof(svrAddr));
    svrAddr.sin_family = AF_INET;
    svrAddr.sin_port = htons(port); // 端口号
    if (inet_pton(AF_INET, strIp.c_str(), &svrAddr.sin_addr) <= 0) {
        close(socket_);
        throw std::runtime_error("Invalid address/ Address not supported! errno=" + to_string(errno));
    }
    int ret = ::connect(socket_, (struct sockaddr*)&svrAddr, sizeof(svrAddr));
    if (ret == -1) {
        if (errno != EINPROGRESS) {
            close(socket_);
            throw std::runtime_error("Connector connect failed! errno=" + to_string(errno));
        }
    }
    loop_->AddEvent(socket_, EPOLL_EVENTS_RW, [this](int fd, uint32_t event) {
        auto pConn = loop_->GetConnection(fd);
        if (pConn == nullptr) {
            std::cout << "Connector::_Connect pConn null. fd=" << fd << ", event=" << event << std::endl;
            return;
        }
        if (event & EPOLLIN){
            pConn->HandleRead(fd, event);
        }
        if (event & EPOLLOUT){
            pConn->HandleWrite(fd, event);
        }
    });
    //添加到管理列表中
    loop_->AddConnection(this);
    //通知连接成功
    connCallback_(this);
}