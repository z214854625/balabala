#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "precompiled.h"
#include "Connector.h"
#include "EventLoop.h"
#include "Connection.h"

using namespace sll;
using namespace std;

Connector::Connector(EventLoop* loop, int port, const std::string& strIp) : socket_(-1), state_(0)
{
    loop_ = loop;
    _Connect(port, strIp);
}

Connector::~Connector()
{
}

void Connector::OnRecv(RecvCallback&& callback)
{
    recvCallback_ = std::forward<RecvCallback>(callback);
}

void Connector::Send(const char* pData, int nLen)
{
    sendMQ_.push(std::string(pData, nLen));
}

void Connector::OnConnected(ConnCallback&& callback)
{
    connCallback_ = std::forward<ConnCallback>(callback);
}

void Connector::HandleRead(int fd, uint32_t events)
{
    if ((events & EPOLLIN) == 0) {
        std::cout << "HandleRead events error." << events << ", fd= " << fd << std::endl;
        return;
    }
    char buffer[BUFF_SIZE] = {0};
    while (true) {
        memset(buffer, 0, BUFF_SIZE);
        int n = recv(fd, buffer, BUFF_SIZE, 0);
        if (n < 0) {
            if(errno != EINTR) {
                std::cout << "read failed! errno= " << errno << ", fd= " << fd << std::endl;
                return;
            }
            break;
        } else if (n == 0) {
            std::cout << "socket disconnected! fd= " << fd << std::endl;
            close(fd);
            return;
        }
        //std::cout << "Received: " << buffer << std::endl;
        recvCallback_(buffer, n);
    }
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_RW);
}

void Connector::HandleWrite(int fd, uint32_t events)
{
    if ((events & EPOLLOUT) == 0) {
        std::cout << "HandleWrite events error." << events << ", fd= " << fd << std::endl;
        return;
    }
    auto& msg = sendMQ_.front();
    sendMQ_.pop();
    int offset = 0;
    int len = msg.size();
    const char* pData = msg.data();
    while (len > 0) {
        int n = write(fd, pData + offset, len);
        if (n == -1) {
            if(errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("HandleWrite");
                return;
            }
            break;
        }
        else if(n == 0) {
            perror("connection close");
            close(fd);
            return;
        }
        else {
            offset += n;
            len -= n;
        }
    }
    //设置读状态
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_RW);
}

int Connector::_Connect(int port, const std::string& strIp)
{
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        std::cerr << "Error creating socket" << std::endl;
        return -1;
    }
    Connection::SetNonBlocking(socket_);
    Connection::SetSockOpt(socket_);
    //设置ip和端口
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port); // 端口号
    if (inet_pton(AF_INET, strIp.c_str(), &server_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address/ Address not supported" << std::endl;
        close(socket_);
        return -1;
    }
    int ret = ::connect(socket_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        std::cerr << "Connection failed" << std::endl;
        close(socket_);
        return -1;
    }
    loop_->AddEvent(socket_, EPOLL_EVENTS_RW, [this](int fd, uint32_t event) {
        if (event & EPOLLIN){
            HandleRead(fd, event);
        }
        if (event & EPOLLOUT){
            HandleWrite(fd, event);
        }
    });
    //通知连接成功
    connCallback_(this);
    return 0;
}