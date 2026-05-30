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

ConnectionBase::ConnectionBase(EventLoop* loop) : socket_(-1), state_(0), loop_(loop)
{
}

ConnectionBase::~ConnectionBase()
{
}

// [2026.5] 私有辅助：在 loop 线程内尝试直发，发不完进 outputBuffer_
// fast-path 与 slow-path（大/小包）共用此函数，避免 write 循环 + 兜底逻辑重复。
// 必须在 loop 线程内调用。
void ConnectionBase::sendInLoop(int fd, const char* p, size_t len)
{
    assert(loop_->IsInLoopThread() && "sendInLoop must be called in loop thread");

    size_t offset = 0;
    if (outputBuffer_.Empty() && CanWriteDirectly()) {
        while (offset < len) {
            ssize_t n = ::write(fd, p + offset, len - offset);
            if (n > 0) {
                offset += n;
            } else if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                std::cerr << "ConnectionBase::sendInLoop write failed! fd=" << fd
                          << ", errno=" << errno << std::endl;
                return;
            } else {
                // n == 0 极少见，谨慎退出避免死循环
                break;
            }
        }
    }
    if (offset < len) {
        // 未发完，剩余字节入 outputBuffer_ + 开 EPOLLOUT
        outputBuffer_.Append(p + offset, len - offset);
        loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
    }
    // 全部写完不需改 epoll（此时通常已是 R 状态）
}

void ConnectionBase::Send(const char* pData, int nLen)
{
    // 入参校验：避免 nullptr/非法长度引发崩溃
    if (pData == nullptr || nLen <= 0) {
        std::cerr << "ConnectionBase::Send invalid args. pData=" << (void*)pData
                  << ", nLen=" << nLen << std::endl;
        return;
    }
    if (socket_ < 0) {
        std::cerr << "ConnectionBase::Send invalid socket. nLen=" << nLen << std::endl;
        return;
    }

    int fd = socket_;

    // ============================================================
    //  Fast-path: 已在 loop 线程
    //    → 直接调 sendInLoop 处理（写不完时自动入 outputBuffer_）
    //    → 零分配（数据写完即返回）
    //
    //  Slow-path: 跨线程
    //    → 必须深拷贝一份数据投递到 loop 线程
    //    → 根据大小选择存储载体：
    //      - 小包（< kBufferThreshold）→ std::string（SSO 友好或单次小 malloc）
    //      - 大包（≥ kBufferThreshold）→ MessageBuffer（make_shared 合并分配）
    // ============================================================
    if (loop_->IsInLoopThread()) {
        sendInLoop(fd, pData, (size_t)nLen);
        return;
    }

    if ((size_t)nLen >= ActorMessage::kBufferThreshold) {
        // 大包：MessageBuffer 路径
        auto buf = MakeBuffer(pData, (size_t)nLen);
        loop_->RunInLoop([this, fd, buf = std::move(buf)]() mutable {
            sendInLoop(fd, buf->Data(), buf->Size());
        });
    } else {
        // 小包：std::string 路径
        std::string data(pData, nLen);
        loop_->RunInLoop([this, fd, data = std::move(data)]() mutable {
            sendInLoop(fd, data.data(), data.size());
        });
    }
}

void ConnectionBase::HandleRead(int fd, uint32_t events)
{
    // 不变量：HandleRead 必须在 loop 线程执行（由 EventLoop 的 cb 调用）
    assert(loop_->IsInLoopThread() && "HandleRead must be called in loop thread");

    // ET 模式下需要一次读到 EAGAIN
    char buffer[NET_BUFF_SIZE];
    while (true) {
        // 不再 memset：recv 返回 n 字节，构造 std::string 只取前 n 字节
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
        // 使用 ActorMessage::Make 自动按大小选路径：
        //   - 小包（< kBufferThreshold）走 std::string（SSO 友好）
        //   - 大包走 sharedBuf（跨 Actor 转发零拷贝）
        auto* actorSys = loop_->GetActorSystem();
        if (actorSys) {
            actorSys->SendByFd(fd, ActorMessage::Make(MsgType::NetworkRecv, 0, fd,
                                                       buffer, n));
        }
    }
    // ET 模式下读完数据后不修改 epoll 事件，保持当前监听状态
    // 当业务层调用 Send() 时会自动添加 EPOLLOUT
}

void ConnectionBase::HandleWrite(int fd, uint32_t events)
{
    // 不变量：HandleWrite 必须在 loop 线程执行
    assert(loop_->IsInLoopThread() && "HandleWrite must be called in loop thread");

    // 一次性把 outputBuffer_ 中所有 readable 数据写出
    // Buffer 数据是连续的，相比原 sendMQ_ 的逐条 pop 一次 write 即可
    while (!outputBuffer_.Empty()) {
        size_t readable = outputBuffer_.ReadableBytes();
        const char* pData = outputBuffer_.Peek();
        int n = ::write(fd, pData, readable);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 内核发送缓冲区满，剩余数据留在 buffer 里下一次 EPOLLOUT 继续
                // 仍需继续监听写事件
                loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
                return;
            } else {
                std::cerr << "Connection write failed! fd=" << fd << ", errno=" << errno << std::endl;
                return;
            }
        } else if (n == 0) {
            // write 返回 0 不代表断连，谨慎退出避免死循环
            break;
        } else {
            // 仅移动 readerIndex_，零拷贝
            outputBuffer_.Retrieve(static_cast<size_t>(n));
        }
    }
    // 全部写完，恢复只读监听
    loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_R);
}
