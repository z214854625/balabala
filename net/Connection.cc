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
    _Listen(port);
}

Connection::~Connection()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Connection::HandleRead(int fd, uint32_t events)
{
    char buffer[NET_BUFF_SIZE] = {0};
    while (true) {
        memset(buffer, 0, NET_BUFF_SIZE);
        int n = recv(fd, buffer, NET_BUFF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) { //被信号中断，继续读取
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) { //数据读取完毕，退出循环
                break;
            } else {
                std::cout << "Connection read failed! fd= " << fd << ", errno= " << errno << std::endl;
                return;
            }
            break;
        } else if (n == 0) {
            std::cout << "Connection close! fd=" << socket_ << std::endl;
            close(fd);
            return;
        }
        //std::cout << "Received: " << buffer << std::endl;
        recvCallback_(buffer, n);
    }
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_W);
}

void Connection::HandleWrite(int fd, uint32_t events)
{
    do
    {
        //std::cout << "Connection::HandleWrite call. fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
        if (sendMQ_.empty()){
            break;
        }
        auto msg = sendMQ_.pop();
        if (!msg){
            //std::cout << "Connection::HandleWrite sendMQ_ pop failed. fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
            break;
        }
        int offset = 0;
        size_t len = (*msg).size();
        const char* pData = (*msg).data();
        while (len > 0) {
            int n = write(fd, pData + offset, len);
            //std::cout << "Connection::HandleWrite write fd= " << fd << ", n="<< n << ", errno=" << errno << std::endl;
            if (n < 0) {
                if (errno == EINTR) { //被信号中断，继续读取
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) { //数据写入完毕，退出循环
                    break;
                } else {
                    std::cout << "Connection write failed! fd= " << fd << ", errno= " << errno << std::endl;
                    return;
                }
                break;
            }
            else if(n == 0) {
                std::cout << "Connection close! fd=" << fd << std::endl;
                close(fd);
                return;
            }
            else {
                offset += n;
                len -= n;
            }
        }

    } while (false);
    //设置读状态
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_R);
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
    if (socket_ < 0) {
        perror("Connection Send socket_ < 0");
        return;
    }
    sendMQ_.push(std::string(pData, nLen));
    loop_->GetPoller()->ModifyEvent(socket_, EPOLL_EVENTS_W);
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

void Connection::_Listen(int port)
{
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ == -1) {
        throw std::runtime_error("Connection create socket failed! errno=" + to_string(errno));
    }
    Connection::SetNonBlocking(socket_);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);
    if (bind(socket_, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == -1) {
        close(socket_);
        throw std::runtime_error("Connection bind failed! errno=" + to_string(errno));
    }
    if (listen(socket_, 5) == -1) {
        close(socket_);
        throw std::runtime_error("Connection listen failed! errno=" + to_string(errno));
    }
    loop_->AddEvent(socket_, EPOLL_EVENTS_R, [this](int fd, uint32_t events) {
        HandleAccept(fd, events);
    });
    std::cout << "listen suc! port= " << port << std::endl;
}

int Connection::SetSockOpt(int fd)
{
    int keepalive = 1;
    int error = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
    if (error != 0) {
        return -1;
    }
    int flags = 1;
    error = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    struct linger ling = {0, 0};
    error = setsockopt(fd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
    if (error != 0){
        return -1;
    }
    flags = 1;
    error = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
    if (error != 0) {
        return -1;
    }
    return 0;
}