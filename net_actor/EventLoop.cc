#include "EventLoop.h"
#include "Epollor.h"
#include "../Util/LockGuard.h"
#include <sys/eventfd.h>
#include <unistd.h>

using namespace bllsll;
using namespace std;

EventLoop::EventLoop()
{
}

EventLoop::~EventLoop()
{
    Stop();
    // 关闭 wakeupFd_（loopThread 已 join，安全关闭）
    if (wakeupFd_ != -1) {
        ::close(wakeupFd_);
        wakeupFd_ = -1;
    }
    // 清理mapConn_中的Connection对象（由EventLoop通过new创建并管理）
    // Acceptor/Connector不在mapConn_中，由TcpServer/TcpClient管理
    for (auto& [fd, pConn] : mapConn_) {
        delete pConn;
    }
    mapConn_.clear();
}

void EventLoop::Create()
{
    poller_.reset(new Epollor(1024));
    if (poller_ == nullptr) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }

    // 创建 eventfd 用于跨线程唤醒/退出
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) {
        throw std::runtime_error("Failed to create eventfd! errno=" + std::to_string(errno));
    }
    // 将 wakeupFd_ 注册到 epoll（此时 loopThread 还没启动，直接调 poller_ 安全）
    poller_->AddEvent(wakeupFd_, EPOLLIN);
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
        callbacks_[wakeupFd_] = [this](int /*fd*/, uint32_t /*events*/) { handleWakeup(); };
    }

    // 启动 epoll loop 线程
    // Wait(-1) 永久阻塞，靠 eventfd 唤醒；彻底消除 25ms 空轮询
    loopThread_ = std::thread(&EventLoop::run, this, -1);
}

void EventLoop::Stop()
{
    bool expected = false;
    if (!stop_.compare_exchange_strong(expected, true)) {
        return; // 已停止
    }
    if (wakeupFd_ != -1) {
        wakeup();
    }
    if (loopThread_.joinable() && loopThread_.get_id() != std::this_thread::get_id()) {
        loopThread_.join();
    }
}

bool EventLoop::IsInLoopThread() const
{
    return std::this_thread::get_id() == loopThreadId_;
}

void EventLoop::wakeup()
{
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    (void)n; // 失败通常是 EAGAIN（计数器满）或关闭中，无害
}

void EventLoop::handleWakeup()
{
    uint64_t cnt = 0;
    ssize_t n = ::read(wakeupFd_, &cnt, sizeof(cnt));
    (void)n;
}

void EventLoop::RunInLoop(PendingTask&& task)
{
    if (IsInLoopThread()) {
        task();
    } else {
        QueueInLoop(std::move(task));
    }
}

void EventLoop::QueueInLoop(PendingTask&& task)
{
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        pendingTasks_.emplace_back(std::move(task));
    }
    // loop 线程内入队不必 wakeup（本轮 runPendingTasks 就会处理）
    if (!IsInLoopThread()) {
        wakeup();
    }
}

void EventLoop::runPendingTasks()
{
    /**
     * 如果是
     * ModifyEvent(RW)
     * ModifyEvent(RW)
     * ModifyEvent(R) 这种情况，会出现最后最后epoll的状态是R，会导致数据发布出去吗？
     * 实际上是不会的，因为设置成R状态，只有再epoll的HandleWrite都写完时，
     * 才会设置成R状态，所以不存在有数据没写完但是状态为R的情况；
     * 还有另外一种情况是handlewrite的同时，外面的worker也再send，这时候如果RW先触发，然后再触发R，
     * 就会出现消息发不出去的情况，但是handleWrite的R在eventloop的线程里面是同步的，所以实际上不存在
     * 先RW再R的情况，要么R+RW(没问题)，要么RW->handleWrite->R(没问题)；
     * 
     * 以上问题虽然没有问题，但是目前代码还是已经优化过了。
     * send()时将sendMQ_.push和ModifyEvent都放到eventloop中执行，
     * 就不存在竞争问题了。
     */
    std::vector<PendingTask> tasks;
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        tasks.swap(pendingTasks_);
    }
    for (auto& t : tasks) {
        try {
            t();
        } catch (const std::exception& e) {
            std::cerr << "[EventLoop] pending task exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[EventLoop] pending task unknown exception" << std::endl;
        }
    }
}

void EventLoop::run(int timeout)
{
    loopThreadId_ = std::this_thread::get_id();
    std::cout << "EventLoop::run timeout=" << timeout << std::endl;
    while (!stop_.load(std::memory_order_acquire)) {
        int ready = poller_->Wait(timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                // 信号中断属于正常情况，继续 loop 而不是退出线程
                continue;
            }
            std::cerr << "epoll_wait error! errno=" << errno << std::endl;
            break;
        }
        // ready == 0 是超时（仅 timeout>=0 时会发生）
        const auto& firedEvents = poller_->GetFiredEvents();
        int len = ready < (int)firedEvents.size() ? ready : (int)firedEvents.size();
        for (int i = 0; i < len; ++i) {
            int fd = firedEvents[i].data.fd;
            uint32_t events = firedEvents[i].events;
            Callback cb;
            {
                bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
                auto it = callbacks_.find(fd);
                if (it != callbacks_.end()) {
                    cb = it->second;
                }
            }
            if (cb) {
                try {
                    cb(fd, events);
                } catch (const std::exception& e) {
                    std::cerr << "[EventLoop] callback exception fd=" << fd
                              << ": " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[EventLoop] callback unknown exception fd=" << fd << std::endl;
                }
            }
        }
        // 处理跨线程投递的任务（AddEvent / ModifyEvent / RemoveEvent / RemoveConnection 等）
        runPendingTasks();
    }
    std::cout << "EventLoop::run end!" << std::endl;
}

void EventLoop::AddEvent(int fd, uint32_t events, Callback&& cb)
{
    // 路由到 loop 线程执行：避免 epoll_ctl 与 epoll_wait 并发，
    // 也避免 callbacks_ 在跨线程下被竞争修改
    auto cbCopy = std::make_shared<Callback>(std::move(cb));
    RunInLoop([this, fd, events, cbCopy]() mutable {
        if (poller_) {
            poller_->AddEvent(fd, events);
        }
        bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
        callbacks_[fd] = std::move(*cbCopy);
    });
}

void EventLoop::ModifyEvent(int fd, uint32_t events, Callback&& cb)
{
    auto cbCopy = std::make_shared<Callback>(std::move(cb));
    RunInLoop([this, fd, events, cbCopy]() mutable {
        if (poller_) {
            try {
                poller_->ModifyEvent(fd, events);
            } catch (const std::exception& e) {
                std::cerr << "[EventLoop] ModifyEvent failed fd=" << fd
                          << ": " << e.what() << std::endl;
                return;
            }
        }
        bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
        callbacks_[fd] = std::move(*cbCopy);
    });
}

void EventLoop::ModifyEventKeepCallback(int fd, uint32_t events)
{
    // 不动回调，仅改 epoll 事件。Send/HandleWrite 路径调用，避免每次拷贝回调
    RunInLoop([this, fd, events]() {
        if (!poller_) return;
        // 仅当回调还存在（连接未删）时才修改，避免对已关闭 fd 做 EPOLL_CTL_MOD 报错
        bool exists;
        {
            bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
            exists = callbacks_.find(fd) != callbacks_.end();
        }
        if (!exists) return;
        try {
            poller_->ModifyEvent(fd, events);
        } catch (const std::exception& e) {
            std::cerr << "[EventLoop] ModifyEventKeepCallback failed fd=" << fd
                      << ": " << e.what() << std::endl;
        }
    });
}

void EventLoop::RemoveEvent(int fd)
{
    RunInLoop([this, fd]() {
        if (poller_) {
            try {
                poller_->RemoveEvent(fd);
            } catch (const std::exception&) {
                // 已被关闭或未注册，忽略
            }
        }
        bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
        callbacks_.erase(fd);
    });
}

int64_t EventLoop::GetMilliSeconds() {
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return now_ms.time_since_epoch().count();
}

void EventLoop::AddConnection(IConnection* pConn)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(connSpinLock_);
    mapConn_.insert({pConn->GetFd(), pConn});
    std::cout << "AddConnection fd=" << pConn->GetFd() << std::endl;
}

IConnection* EventLoop::GetConnection(int fd)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(connSpinLock_);
    auto it = mapConn_.find(fd);
    if (it == mapConn_.end()) {
        return nullptr;
    }
    return it->second;
}

void EventLoop::RemoveConnection(int fd)
{
    // 用 QueueInLoop（而不是 RunInLoop）入队：
    // - 即使当前在 loop 线程的回调内调用（如 HandleRead 自删），
    //   实际删除也会在本次回调返回后才执行，杜绝 UAF。
    QueueInLoop([this, fd]() {
        // 先摘 epoll、清回调
        if (poller_) {
            try {
                poller_->RemoveEvent(fd);
            } catch (const std::exception&) {
                // 已被关闭，忽略
            }
        }
        {
            bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
            callbacks_.erase(fd);
        }
        // 再删 Connection 对象
        IConnection* pConn = nullptr;
        {
            bllsll::LockGuard<bllsll::SpinLock> lock(connSpinLock_);
            auto it = mapConn_.find(fd);
            if (it != mapConn_.end()) {
                pConn = it->second;
                mapConn_.erase(it);
            }
        }
        if (pConn) {
            delete pConn;
            std::cout << "RemoveConnection fd=" << fd << std::endl;
        }
    });
}
