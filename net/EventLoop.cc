#include "EventLoop.h"
#include "Epollor.h"
#include "../Util/LockGuard.h"

using namespace sll;
using namespace std;

EventLoop::EventLoop() : stop_(false)
{
}

EventLoop::~EventLoop()
{
    stop_ = true;
    for (auto &th : threadPool_) {
        if (th.joinable()) {
            th.join();
        }
    }
}

void EventLoop::Create(int threadSize)
{
    //poller_ = std::make_unique<Epollor>(1024);
    poller_.reset(new Epollor(1024));

    if (poller_ == nullptr) {
        throw std::runtime_error("Failed to create epoll file descriptor");
    }
    //队列任务线程
    for (int i = 0; i < threadSize; i++) {
        threadPool_.emplace_back(std::thread(&EventLoop::WorkerThread, this));
    }
    //loop线程
    threadPool_.emplace_back(std::thread(&EventLoop::run, this, 25));
}

void EventLoop::WorkerThread()
{
    std::cout << "EventLoop::WorkerThread 1--" << std::endl;
    while (true) {
        if(stop_){
            while(!taskQueue_.empty()) {
                auto task = taskQueue_.pop();
                if (task) {
                    (*task)();
                }
            }
            return;
        }
        if (taskQueue_.empty()){
            usleep(25000);
            continue;
        }
        auto task = taskQueue_.pop();
        if (task) {
            (*task)();
        }
    }
    std::cout << "EventLoop::WorkerThread end!" << std::endl;
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
        //std::cout << "EventLoop::run event size=" << len << std::endl;
        for (int i = 0; i < len; ++i) {
            int fd = firedEvents[i].data.fd;
            uint32_t events = firedEvents[i].events;
            sll::LockGuard<sll::SpinLock> lock(spinLock_); //对callbacks_上锁
            auto it = callbacks_.find(fd);
            //std::cout << "EventLoop::run fd= " << fd << ", event= " << events << ", find= " << (it != callbacks_.end()) << std::endl;
            if (it != callbacks_.end()) {
                std::function<void()> task = [it, fd, events]() { it->second(fd, events); };
                {
                    taskQueue_.push(task);
                    //std::cout << "EventLoop::run task enqueue. " << fd << std::endl;
                }
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
    sll::LockGuard<sll::SpinLock> lock(spinLock_);
    callbacks_[fd] = std::forward<Callback>(cb);
    //std::cout << "EventLoop::AddEvent " << fd << std::endl;
}

void EventLoop::ModifyEvent(int fd, uint32_t events, Callback&& cb)
{
    if(poller_) {
        poller_->ModifyEvent(fd, events);
    }
    sll::LockGuard<sll::SpinLock> lock(spinLock_);
    callbacks_[fd] = std::forward<Callback>(cb);
    //std::cout << "EventLoop::ModifyEvent " << fd << std::endl;
}

void EventLoop::RemoveEvent(int fd)
{
    if(poller_) {
        poller_->RemoveEvent(fd);
    }
    sll::LockGuard<sll::SpinLock> lock(spinLock_);
    callbacks_.erase(fd);
    //std::cout << "EventLoop::RemoveEvent " << fd << std::endl;
}

void EventLoop::OnDispatch(int timeout)
{
    //std::cout << "EventLoop::OnDispatch timeout= " << timeout << std::endl;
    int64_t tick1 = GetMilliSeconds();
    while (true) {
        if (msgQueue_.empty()){
            return;
        }
        auto p = msgQueue_.pop();
        if (p) {
            auto fd = std::get<0>(*p);
            auto& msg = std::get<1>(*p);
            auto& callback = std::get<2>(*p);
            auto pConn = this->GetConnection(fd);
            if (!pConn) {
                std::cout << "EventLoop::OnDispatch pConn null, fd=" << fd << ", msg_size=" << msg.size() << std::endl;
                continue;
            }
            callback(pConn, msg.c_str(), msg.size());
        }
        int64_t tick2 = GetMilliSeconds();
        if (timeout > 0 && ((tick2 - tick1) > timeout)){
            return;
        }
    }
}

int64_t EventLoop::GetMilliSeconds(){
    auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    auto val = now_ms.time_since_epoch().count();
    return val;
}

void EventLoop::AddMsg(recvMsgType&& p)
{
    msgQueue_.push(std::forward<recvMsgType>(p));
}

void EventLoop::AddConnection(IConnection* pConn)
{
    mapConn_.insert({pConn->GetFd(), pConn});
    std::cout << "AddConnection fd=" << pConn->GetFd() << std::endl;
}

IConnection* EventLoop::GetConnection(int fd)
{
    auto it = mapConn_.find(fd);
    if (it == mapConn_.end()) {
        return nullptr;
    }
    return it->second;
}

void EventLoop::RemoveConnection(int fd)
{
    auto it = mapConn_.find(fd);
    if (it != mapConn_.end()) {
        delete it->second;
        mapConn_.erase(it);
        std::cout << "RemoveConnection fd=" << fd << std::endl;
    }
}