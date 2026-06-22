#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include "Connector.h"
#include "Poller.h"
#include "EventLoop.h"
#include "ActorSystem.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

Connector::Connector(EventLoop* loop, int port, const std::string& strIp, uint32_t ownerActorId)
    : ConnectionBase(loop), port_(port), strIp_(strIp), ownerActorId_(ownerActorId)
{
    _Connect(port, strIp);
}

Connector::~Connector()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Connector::Send(const char* pData, int nLen)
{
    ConnectionBase::Send(pData, nLen);
}

void Connector::HandleRead(int fd, uint32_t events)
{
    if (connecting_) {
        // 连接阶段不应收到读事件，忽略
        return;
    }
    ConnectionBase::HandleRead(fd, events);
}

void Connector::HandleWrite(int fd, uint32_t events)
{
    if (connecting_) {
        // 非阻塞connect完成，检查连接是否成功
        connecting_ = false;
        int err = 0;
        socklen_t errLen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0 || err != 0) {
            std::cerr << "Connector connect failed! fd=" << fd << ", err=" << err << std::endl;
            // [P1-1 修复] 连接失败时通知 ownerActor
            auto* actorSys = loop_->GetActorSystem();
            if (actorSys) {
                actorSys->Send(ownerActorId_, ActorMessage{MsgType::Disconnected, 0, fd, "connect_failed"});
            }
            loop_->RemoveEvent(fd);
            close(socket_);
            socket_ = -1;
            return;
        }
        std::cout << "Connector connected! fd=" << fd << std::endl;

        // 绑定fd到ownerActor，并发送Connected消息
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            actorSys->BindFdToActor(fd, ownerActorId_);
            actorSys->Send(ownerActorId_, ActorMessage{MsgType::Connected, 0, fd, ""});
        }

        // 检查连接建立前是否有 Send() 数据入队（已在 loop 线程，安全访问 outputBuffer_）
        // 此时已在 loop 线程（HandleWrite 由 EventLoop 触发），ModifyEventKeepCallback 同步执行
        if (!outputBuffer_.Empty()) {
            loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
        } else {
            // 切换到读模式，等待服务端数据（Send() 会自动添加 EPOLLOUT）
            loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_R);
        }
        return;
    }
    ConnectionBase::HandleWrite(fd, events);
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
    if (ret == 0) {
        // 连接立即成功（本地连接可能出现）
        connecting_ = false;
    } else if (errno == EINPROGRESS) {
        // 非阻塞连接进行中，等待EPOLLOUT确认
        connecting_ = true;
    } else {
        close(socket_);
        throw std::runtime_error("Connector connect failed! errno=" + to_string(errno));
    }

    // 注册epoll事件
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
    //添加到管理列表中（裸指针，由 TcpClient 管理生命周期）
    loop_->AddConnectionRaw(this);

    if (!connecting_) {
        // 连接已立即成功，绑定fd并通知Actor
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            actorSys->BindFdToActor(socket_, ownerActorId_);
            actorSys->Send(ownerActorId_, ActorMessage{MsgType::Connected, 0, socket_, ""});
        }
        loop_->ModifyEventKeepCallback(socket_, EPOLL_EVENTS_R);
    }
    // 如果connecting_==true，HandleWrite中EPOLLOUT触发时会检查连接结果并回调
}
