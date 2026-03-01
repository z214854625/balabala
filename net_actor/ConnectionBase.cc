#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "ConnectionBase.h"
#include "EventLoop.h"
#include "ActorSystem.h"
#include "Message.h"
#include "Poller.h"

using namespace bllsll;
using namespace std;

ConnectionBase::ConnectionBase(EventLoop* loop) : socket_(-1), state_(0), lastMsgCache_(""), loop_(loop)
{
}

ConnectionBase::~ConnectionBase()
{
}

void ConnectionBase::Send(const char* pData, int nLen)
{
    if (socket_ < 0) {
        std::cout << "ConnectionBase::Send invaild socket. nLen=" << nLen << std::endl;
        return;
    }
    sendMQ_.push(std::string(pData, nLen));
    loop_->GetPoller()->ModifyEvent(socket_, EPOLL_EVENTS_RW);
}

void ConnectionBase::HandleRead(int fd, uint32_t events)
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
            std::cout << "Connection close! fd=" << fd << std::endl;
            // 发送Disconnected消息给绑定的Actor
            auto* actorSys = loop_->GetActorSystem();
            if (actorSys) {
                actorSys->SendByFd(fd, ActorMessage{MsgType::Disconnected, 0, fd, ""});
                actorSys->UnbindFd(fd);
            }
            // RemoveConnection 内部会先 RemoveEvent(fd) 再 delete Connection
            loop_->RemoveConnection(fd);
            return;
        }
        std::string str(buffer, n);
        // 发送NetworkRecv消息给绑定的Actor（替代原来的callback机制）
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            actorSys->SendByFd(fd, ActorMessage{MsgType::NetworkRecv, 0, fd, std::move(str)});
        }
    }
    // ET模式下读完数据后不修改epoll事件，保持当前监听状态
    // 当业务层调用Send()时会自动添加EPOLLOUT
}

void ConnectionBase::HandleWrite(int fd, uint32_t events)
{
    std::cout << "HandleWrite fd= " << fd << ", mq_size="<< sendMQ_.size() << ", cache_size= " << lastMsgCache_.size() << std::endl;
    while(!sendMQ_.empty() || !lastMsgCache_.empty())
    {
        std::string strMsg = "";
        if (!lastMsgCache_.empty()) {
            strMsg = std::move(lastMsgCache_);
        }
        else {
            auto msg = sendMQ_.pop();
            if (!msg){
                std::cout << "HandleWrite msg nullopt! fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
                continue;
            }
            strMsg = std::move(*msg);
        }
        if (strMsg.empty()) {
            std::cout << "HandleWrite strMsg empty! fd= " << fd << ", mq size="<< sendMQ_.size() << std::endl;
            continue;
        }
        int offset = 0;
        size_t len = strMsg.size();
        const char* pData = strMsg.c_str();
        while (len > 0) {
            int n = write(fd, pData + offset, len);
            if (n < 0) {
                std::cout << "Connection write fd= " << fd << ", n="<< n << ", errno=" << errno << std::endl;
                if (errno == EINTR) { //被信号中断，继续读取
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) { //数据写入完毕，退出循环
                    std::cout << "cache overflow! fd=" << fd << ", len="<< len  << std::endl;
                    //如果缓冲区满了，需要继续触发写事件
                    if (len > 0){
                        lastMsgCache_.clear();
                        lastMsgCache_ = std::move(string(pData + offset, len));
                    }
                    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_RW);
                    return;
                } else {
                    std::cout << "Connection write failed! fd= " << fd << ", errno= " << errno << std::endl;
                    return;
                }
            }
            else if(n == 0) {
                // write()返回0通常表示写入了0字节，不代表断连
                std::cout << "Connection write 0 bytes! fd=" << fd << ", remaining=" << len << std::endl;
                break;
            }
            else {
                offset += n;
                len -= n;
            }
        }
    }
    //设置读状态
    loop_->GetPoller()->ModifyEvent(fd, EPOLL_EVENTS_R);
}
