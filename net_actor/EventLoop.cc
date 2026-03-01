#include "EventLoop.h"
#include "Epollor.h"
#include "../Util/LockGuard.h"

using namespace bllsll;
using namespace std;

EventLoop::EventLoop() : stop_(false)
{
}

EventLoop::~EventLoop()
{
    stop_ = true;
    if (loopThread_.joinable()) {
        loopThread_.join();
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
    // 只启动一个epoll loop线程，所有I/O操作都在这个线程中串行执行
    loopThread_ = std::thread(&EventLoop::run, this, 25);
}

void EventLoop::run(int timeout)
{
    std::cout << "EventLoop::run timeout= "<< timeout << std::endl;
    while (!stop_) {
        int ready = poller_->Wait(timeout);
        if (ready < 0) {
            if (errno == EINTR) { // Interrupted by signal
                std::cout << "epoll interrupted by signal." << std::endl;
                return;
            }
            std::cout << "epoll_wait error! errno=" << errno << std::endl;
            return;
        }
        else if (ready == 0) { // time out
           continue;
        }
        const auto& firedEvents = poller_->GetFiredEvents();
        int len = (ready < (int)firedEvents.size() ?  ready : firedEvents.size());
        for (int i = 0; i < len; ++i) {
            int fd = firedEvents[i].data.fd;
            uint32_t events = firedEvents[i].events;
            // 拷贝回调，避免迭代器失效问题
            Callback cb;
            {
                bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
                auto it = callbacks_.find(fd);
                if (it != callbacks_.end()) {
                    cb = it->second; // 拷贝回调函数
                }
            }
            // 直接在loop线程中执行I/O回调，保证同一fd的read/write串行
            if (cb) {
                cb(fd, events);
            }
        }
    }
    std::cout << "EventLoop::run end!" << std::endl;
}

void EventLoop::AddEvent(int fd, uint32_t events, Callback&& cb)
{
    if(poller_) {
        poller_->AddEvent(fd, events);
    }
    bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
    callbacks_[fd] = std::move(cb);
}

void EventLoop::ModifyEvent(int fd, uint32_t events, Callback&& cb)
{
    if(poller_) {
        poller_->ModifyEvent(fd, events);
    }
    bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
    callbacks_[fd] = std::move(cb);
}

void EventLoop::RemoveEvent(int fd)
{
    if(poller_) {
        poller_->RemoveEvent(fd);
    }
    bllsll::LockGuard<bllsll::SpinLock> lock(cbSpinLock_);
    callbacks_.erase(fd);
}

int64_t EventLoop::GetMilliSeconds(){
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto val = now_ms.time_since_epoch().count();
    return val;
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
    // 先移除epoll事件和回调，再删除连接对象
    RemoveEvent(fd);
    bllsll::LockGuard<bllsll::SpinLock> lock(connSpinLock_);
    auto it = mapConn_.find(fd);
    if (it != mapConn_.end()) {
        delete it->second;
        mapConn_.erase(it);
        std::cout << "RemoveConnection fd=" << fd << std::endl;
    }
}
