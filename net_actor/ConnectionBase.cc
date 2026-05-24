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

    // ============================================================
    //  Fast-path: 已在 loop 线程 + 无积压 + 可直发
    //    → 直接尝试 write socket，零 std::string 构造、零 task 入队、
    //      零 wakeup 系统调用。完全发完即可立即返回；只有部分发完时才把
    //      剩余字节 Append 到 outputBuffer_ 等下次 EPOLLOUT。
    //
    //  Slow-path: 跨线程 / 已有积压 / 连接未建立
    //    → 把"Append + ModifyEvent(RW)"打包成 task 投递到 loop 线程，
    //      与 HandleWrite 串行执行，消除并发 race。
    // ============================================================
    int fd = socket_;
    if (loop_->IsInLoopThread() && outputBuffer_.Empty() && CanWriteDirectly()) {
        // ---- Fast-path ----
        ssize_t written = 0;
        while (written < nLen) {
            ssize_t n = ::write(fd, pData + written, nLen - written);
            if (n > 0) {
                written += n;
            } else if (n < 0) {
                if (errno == EINTR) {
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 内核缓冲区满，剩余部分入 buffer 走 EPOLLOUT
                    break;
                } else {
                    std::cerr << "ConnectionBase::Send write failed! fd=" << fd
                              << ", errno=" << errno << std::endl;
                    return;
                }
            } else {
                // n == 0 极少见，谨慎处理：不再尝试，剩余入 buffer
                break;
            }
        }
        if (written < nLen) {
            // 没发完，剩余字节进 buffer + 开 EPOLLOUT
            outputBuffer_.Append(pData + written, nLen - written);
            loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
        }
        // 全部写完则不需改 epoll（此时通常已是 R 状态，EPOLLOUT 也无碍）
        return;
    }

    // ---- Slow-path ----
    // 把数据 + ModifyEvent 打包成 task 投递到 loop 线程，与 HandleWrite 串行
    std::string data(pData, nLen);
    loop_->RunInLoop([this, fd, data = std::move(data)]() mutable {
        // 已在 loop 线程执行：再做一次直发尝试（CanWriteDirectly 此时可能已就绪，
        // 比如 Connector 连接刚完成）。否则走 buffer 路径。
        bool tryDirect = outputBuffer_.Empty() && CanWriteDirectly();
        size_t offset = 0;
        if (tryDirect) {
            while (offset < data.size()) {
                ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);
                if (n > 0) {
                    offset += n;
                } else if (n < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    std::cerr << "ConnectionBase::Send (slow-path direct) write failed! fd="
                              << fd << ", errno=" << errno << std::endl;
                    return;
                } else {
                    break;
                }
            }
        }
        if (offset < data.size()) {
            outputBuffer_.Append(data.data() + offset, data.size() - offset);
            loop_->ModifyEventKeepCallback(fd, EPOLL_EVENTS_RW);
        }
    });
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
