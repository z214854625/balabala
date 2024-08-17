#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "precompiled.h"
#include "Poller.h"
#include "Connector.h"
#include "EventLoop.h"

using namespace sll;
using namespace std;

Connector::Connector(EventLoop* loop, int port, const std::string& strIp) 
    : socket_(-1), state_(0), loop_(loop), port_(port), strIp_(strIp)
{
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
    if (socket_ < 0) {
        std::cout << "Connector::Send failed! socket_= " << socket_ << std::endl;
        return;
    }
    sendMQ_.push(std::string(pData, nLen));
    loop_->GetPoller()->ModifyEvent(socket_, EPOLL_EVENTS_W);
}

void Connector::OnConnected(ConnCallback&& callback)
{
    connCallback_ = std::forward<ConnCallback>(callback);
    _Connect(port_, strIp_);
}

void Connector::HandleRead(int fd, uint32_t events)
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
                std::cout << "Connector read failed! fd= " << fd << ", errno= " << errno << std::endl;
                return;
            }
            break;
        } else if (n == 0) {
            std::cout << "Connector close! fd=" << socket_ << std::endl;
            close(fd);
            return;
        }
        //std::cout << "Received: " << buffer << std::endl;
        recvCallback_(buffer, n);
    }
    //设置写状态
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_RW);
}

void Connector::HandleWrite(int fd, uint32_t events)
{
    do
    {
        //std::cout << "Connector::HandleWrite call. fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
        if (sendMQ_.empty()){
            break;
        }
        auto msg = sendMQ_.pop();
        if (!msg){
            //std::cout << "Connector::HandleWrite sendMQ_ pop failed. fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
            break;
        }
        int offset = 0;
        size_t len = (*msg).size();
        const char* pData = (*msg).data();
        while (len > 0) {
            int n = write(fd, pData + offset, len);
            //std::cout << "Connector::HandleWrite write fd= " << fd << ", n="<< n << ", errno=" << errno << std::endl;
            if (n < 0) {
                if (errno == EINTR) { //被信号中断，继续读取
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) { //数据写入完毕，退出循环
                    break;
                } else {
                    std::cout << "Connector write failed! fd= " << fd << ", errno= " << errno << std::endl;
                    return;
                }
                break;
            }
            else if(n == 0) {
                std::cout << "Connector close! fd=" << fd << std::endl;
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
        if (event & EPOLLIN){
            HandleRead(fd, event);
        }
        if (event & EPOLLOUT){
            HandleWrite(fd, event);
        }
    });
    //通知连接成功
    connCallback_(this);
}