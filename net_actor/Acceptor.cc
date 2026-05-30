#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "Acceptor.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "Connection.h"
#include "IOHelper.h"
#include "ActorSystem.h"
#include "Message.h"

using namespace bllsll;
using namespace std;

Acceptor::Acceptor(int port, EventLoop* loop, uint32_t listenerActorId)
    : socket_(-1), loop_(loop), listenerActorId_(listenerActorId)
{
    _Listen(port);
}

Acceptor::~Acceptor()
{
    if (socket_ != -1) {
        close(socket_);
    }
}

void Acceptor::HandleAccept(int listenFd, uint32_t events)
{
    if ((events & EPOLLIN) == 0) {
        std::cout << "HandleAccept events error." << events << ", fd= " << listenFd << std::endl;
        return;
    }

    // ET 模式下必须循环 accept 直到 EAGAIN，否则一次 epoll 通知只 accept 一个，
    // 高并发时大量连接会卡在 accept queue 里得不到处理。
    while (true) {
        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        int clientFd = ::accept(listenFd, (sockaddr*)&addr, &addrLen);
        if (clientFd == -1) {
            if (errno == EINTR) continue;                       // 被信号中断，重试
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // accept queue 已空，正常退出循环
            std::cerr << "HandleAccept accept failed! fd= " << listenFd
                      << ", errno=" << errno << std::endl;
            break;
        }
        std::cout << "Accepted new connection. clientFd=" << clientFd << std::endl;
        IOHelper::SetNonBlocking(clientFd);

        // [2026.5 多 Reactor] 选择 fd 归属的 loop：
        //   - 有 SubReactor 池且非空 → Round-Robin 取一个 sub loop
        //   - 否则 → 仍用主 loop（保持单 Reactor 行为）
        EventLoop* targetLoop = loop_;
        if (threadPool_ && threadPool_->Enabled()) {
            EventLoop* sub = threadPool_->GetNextLoop();
            if (sub) targetLoop = sub;
        }

        IConnection* pNewConn = new Connection(clientFd, targetLoop);
        if (pNewConn == nullptr) {
            std::cout << "HandleAccept pNewConn null. clientFd=" << clientFd << std::endl;
            continue;
        }
        targetLoop->AddConnection(pNewConn);

        // 绑定fd到监听Actor，并发送Connected消息
        // 注意：actorSys 总是用主 loop 上的（同一个 ActorSystem 实例）
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            // [2026.5] 把 fd 所属的 loop 也注册进 ActorSystem，便于 Actor::SendToNetwork 查 loop
            actorSys->BindFdToActor(clientFd, listenerActorId_, targetLoop);
            actorSys->Send(listenerActorId_, ActorMessage{MsgType::Connected, 0, clientFd, ""});
        }

        // 在 fd 归属的 loop 上注册 epoll 事件
        // targetLoop 可能不是当前线程的 loop，AddEvent 内部已用 RunInLoop 路由
        targetLoop->AddEvent(clientFd, EPOLL_EVENTS_RW, [targetLoop](int fd, uint32_t event) {
            auto pConn = targetLoop->GetConnection(fd);
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
    // backlog 取 SOMAXCONN（Linux 上通常 4096），
    // 应对短时间大量并发 connect 的场景。
    // 原值 5 在 16+ 并发连接时会丢失新连接。
    if (listen(socket_, SOMAXCONN) == -1) {
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
