#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "precompiled.h"
#include "Connection.h"
#include "EventLoop.h"

using namespace sll;
using namespace std;

Connection::Connection(int port, EventLoop* loop) : socket_(-1), state_(0), loop_(loop)
{
    Listen(port);
}

Connection::~Connection()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Connection::HandleRead(int fd, uint32_t events)
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

void Connection::HandleWrite(int fd, uint32_t events)
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

void Connection::HandleAccept(int listenFd, uint32_t events)
{
    if ((events & EPOLLIN) == 0) {
        std::cout << "HandleAccept events error." << events << ", fd= " << listenFd << std::endl;
        return;
    }
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    int clientFd = ::accept(listenFd, (sockaddr*)&addr, &addrLen);
    if (clientFd == -1) {
        perror("accept clientFd -1");
        return;
    }
    SetNonBlocking(clientFd);
    std::cout << "Accepted new connection. clientFd=" << clientFd << std::endl;
    loop_->AddEvent(clientFd, EPOLL_EVENTS_RW, [this](int fd, uint32_t event) {
        if (event & EPOLLIN){
            HandleRead(fd, event);
        }
        if (event & EPOLLOUT){
            HandleWrite(fd, event);
        }
    });
}

void Connection::HandleClose()
{
    close(socket_);
    socket_ = -1;
}

void Connection::OnRecv(RecvCallback&& callback)
{
    recvCallback_ = std::forward<RecvCallback>(callback);
}

void Connection::Send(const char* pData, int nLen)
{
    sendMQ_.push(std::string(pData, nLen));
}

void Connection::SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void Connection::Close()
{
    HandleClose();
}

int Connection::Listen(int port)
{
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == -1) {
        perror("socket");
        return -1;
    }
    Connection::SetNonBlocking(socket_);
    SetSockOpt(socket_);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if (bind(socket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        perror("bind");
        close(socket_);
        return -1;
    }
    if (listen(socket_, 5) == -1) {
        perror("listen");
        close(socket_);
        return -1;
    }
    loop_->AddEvent(socket_, EPOLL_EVENTS_R, [this](int fd, uint32_t events) {
        HandleAccept(fd, events);
    });
    return 0;
}

int Connection::SetSockOpt(int fd)
{
    int flags =1;
    int keepalive = 1;
    struct linger ling = {0, 0};

    int error = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
    if (error != 0) {
        return -1;
    }
    error = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    error = setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
    if (error != 0){
        return -1;
    }
    error = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    return 0;
}

/*
int main() {
    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd == -1) {
        perror("socket");
        return 1;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(12345);

    if (bind(listenFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        perror("bind");
        close(listenFd);
        return 1;
    }

    if (listen(listenFd, 5) == -1) {
        perror("listen");
        close(listenFd);
        return 1;
    }

    EpollWrapper::setNonBlocking(listenFd);

    try {
        EpollWrapper epoll(10, 4);
        epoll.addFd(listenFd, EPOLLIN | EPOLLET, 
                    [&](int fd, uint32_t events) { handleAccept(fd, events, epoll); });

        epoll.run();
    } catch (const std::exception &e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    close(listenFd);
    return 0;
}

*/