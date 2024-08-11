#include "EventLoop.h"
#include "Epollor.h"

using namespace sll;
using namespace std;

EventLoop::EventLoop() : looping_(false), stop_(false)
{
}

EventLoop::~EventLoop()
{
    stop_ = true;
    cv_.notify_all();
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
    for (int i = 0; i < threadSize; i++) {
        threadPool_.emplace_back(std::thread(&EventLoop::WorkerThread, this));
    }
}

void EventLoop::run(int timeout)
{
    while (!stop_) {
        int ready = poller_->Wait(timeout);
        if (ready < 0) {
            if (errno == EINTR) { // Interrupted by signal
                std::cout << "epoll interrupted by signal." << std::endl;
                return;
            }
            throw std::runtime_error("Error in epoll_wait");
        }
        const auto& firedEvents = poller_->GetFiredEvents();
        for (size_t i = 0; i < firedEvents.size(); ++i) {
            std::lock_guard<std::mutex> lock(mtx_); //对 callbacks_ 加锁
            int fd = firedEvents[i].data.fd;
            uint32_t events = firedEvents[i].events;
            auto it = callbacks_.find(fd);
            if (it != callbacks_.end()) {
                std::function<void()> task = [it, fd, events]() { it->second(fd, events); };
                {
                    std::lock_guard<std::mutex> qlock(mtx_); //对 taskQueue_ 加锁
                    taskQueue_.emplace(task);
                }
                cv_.notify_one();
            }
        }
    }
}

void EventLoop::WorkerThread()
{
    while (!stop_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] { return !taskQueue_.empty() || stop_; });
            if (stop_ && taskQueue_.empty()) return;

            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        task();
    }
}

void EventLoop::AddEvent(int fd, uint32_t events, Callback&& cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->AddEvent(fd, events);
    }
    callbacks_[fd] = std::forward<Callback>(cb);
}

void EventLoop::ModifyEvent(int fd, uint32_t events, Callback&& cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->ModifyEvent(fd, events);
    }
    callbacks_[fd] = std::forward<Callback>(cb);
}

void EventLoop::RemoveEvent(int fd)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->RemoveEvent(fd);
    }
    callbacks_.erase(fd);
}
