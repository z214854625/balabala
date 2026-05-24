#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cassert>
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
    // 入参校验：避免 nullptr/非法长度引发 std::string 构造崩溃
    if (pData == nullptr || nLen <= 0) {
        std::cerr << "ConnectionBase::Send invalid args. pData=" << (void*)pData
                  << ", nLen=" << nLen << std::endl;
        return;
    }
    if (socket_ < 0) {
        std::cerr << "ConnectionBase::Send invalid socket. nLen=" << nLen << std::endl;
        return;
    }

    // 将 push + ModifyEvent 整体投递到 loop 线程串行执行，与 HandleWrite 完全互斥。
    // 这样可以彻底消除以下 race：
    //   worker push 后 queue task_RW，loop HandleWrite 看到空队列后 sync ModifyEvent(R)
    //   → 最终状态 R 但 sendMQ_ 已有数据 → 卡死
    // 现在 push 和 ModifyEvent 都在 loop 线程，HandleWrite 也在 loop 线程，三者串行。
    int fd = socket_;
    std::string data(pData, nLen);
    loop_->RunInLoop([this, fd, data = std::move(data)]() mutable {
        // 此时已在 loop 线程，与 HandleWrite 互斥
        sendMQ_.push(std::move(data));
        // 直接同步开 EPOLLOUT（IsInLoopThread=true，ModifyEventKeepCallback 同步执行）
        loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
    });
}

void ConnectionBase::HandleRead(int fd, uint32_t events)
{
    // 不变量：HandleRead 必须在 loop 线程执行（由 EventLoop 的 cb 调用）
    assert(loop_->IsInLoopThread() && "HandleRead must be called in loop thread");

    // ET 模式下需要一次读到 EAGAIN
    char buffer[NET_BUFF_SIZE];
    while (true) {
        // 不再 memset(64KB)：recv 返回 n 字节，构造 std::string 只取前 n 字节，
        // 多余的 buffer 区域永远不会被读到，清零纯属浪费
        int n = recv(fd, buffer, NET_BUFF_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) {           // 被信号中断，继续读
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) { // 数据读完
                break;
            } else {
                std::cerr << "Connection read failed! fd=" << fd << ", errno=" << errno << std::endl;
                return;
            }
        } else if (n == 0) {
            std::cout << "Connection close! fd=" << fd << std::endl;
            // 发送 Disconnected 消息给绑定的 Actor
            auto* actorSys = loop_->GetActorSystem();
            if (actorSys) {
                actorSys->SendByFd(fd, ActorMessage{MsgType::Disconnected, 0, fd, ""});
                actorSys->UnbindFd(fd);
            }
            // RemoveConnection 已改为 QueueInLoop 延迟删除：
            // 当前回调返回后才真正 delete Connection，避免 UAF
            loop_->RemoveConnection(fd);
            return;
        }
        // 发送 NetworkRecv 消息给绑定的 Actor
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            actorSys->SendByFd(fd, ActorMessage{MsgType::NetworkRecv, 0, fd,
                                                 std::string(buffer, n)});
        }
    }
    // ET 模式下读完数据后不修改 epoll 事件，保持当前监听状态
    // 当业务层调用 Send() 时会自动添加 EPOLLOUT
}

void ConnectionBase::HandleWrite(int fd, uint32_t events)
{
    // 不变量：HandleWrite 必须在 loop 线程执行
    // 这条不变量是 Send 路径正确性的根基——
    // 一旦在 worker 线程触发，ModifyEvent(R) 会进 pendingTasks_，
    // 与 worker 投的 ModifyEvent(RW) 顺序不定，可能被覆盖造成数据卡死。
    assert(loop_->IsInLoopThread() && "HandleWrite must be called in loop thread");

    while (!sendMQ_.empty() || !lastMsgCache_.empty()) {
        std::string strMsg;
        if (!lastMsgCache_.empty()) {
            strMsg = std::move(lastMsgCache_);
        } else {
            auto msg = sendMQ_.pop();
            if (!msg) {
                // 队列瞬态空：因为 push 由 RunInLoop 串行投递，理论上不会发生。
                // 保险起见仍处理：跳出本轮，由后续 EPOLLOUT 重新驱动。
                break;
            }
            strMsg = std::move(*msg);
        }
        if (strMsg.empty()) {
            continue;
        }
        int offset = 0;
        size_t len = strMsg.size();
        const char* pData = strMsg.c_str();
        while (len > 0) {
            int n = write(fd, pData + offset, len);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 内核发送缓冲区已满，保留剩余字节，下一次 EPOLLOUT 继续
                    if (len > 0) {
                        lastMsgCache_.assign(pData + offset, len);
                    }
                    // 仍需继续监听写事件（同步执行，因为已在 loop 线程）
                    loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
                    return;
                } else {
                    std::cerr << "Connection write failed! fd=" << fd << ", errno=" << errno << std::endl;
                    return;
                }
            } else if (n == 0) {
                // write 返回 0 不代表断连，按已发送 0 字节处理后退出本条
                break;
            } else {
                offset += n;
                len -= n;
            }
        }
    }
    // 全部写完，恢复只读监听。
    // 注意：因为 Send 的 push + ModifyEvent(RW) 已经整体投递到 loop 线程，
    // 与本函数串行执行，所以不存在"sendMQ_ 误判为空"的 race——
    // 只要本函数看到 sendMQ_ 空，那它就是真的空，可以安全地切回 R。
    loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_R);
}
