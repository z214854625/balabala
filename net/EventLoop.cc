#include "EventLoop.h"
#include "Epollor.h"

using namespace sll;
using namespace std;

EventLoop::EventLoop() : stop_(false)
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
    //队列任务线程
    for (int i = 0; i < threadSize; i++) {
        threadPool_.emplace_back(std::thread(&EventLoop::WorkerThread, this));
    }
    //loop线程
    threadPool_.emplace_back(std::thread(&EventLoop::run, this, 25));
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
        std::lock_guard<std::mutex> lock(mtx_);
        for (int i = 0; i < len; ++i) {
            int fd = firedEvents[i].data.fd;
            uint32_t events = firedEvents[i].events;
            auto it = callbacks_.find(fd);
            //std::cout << "EventLoop::run fd= " << fd << ", event= " << events << ", find= " << (it != callbacks_.end()) << std::endl;
            if (it != callbacks_.end()) {
                std::function<void()> task = [it, fd, events]() { it->second(fd, events); };
                {
                    taskQueue_.emplace(task);
                    //std::cout << "EventLoop::run task enqueue. " << fd << std::endl;
                }
                cv_.notify_all();
            }
        }
    }
    std::cout << "EventLoop::run end!" << std::endl;
}

void EventLoop::WorkerThread()
{
    std::cout << "EventLoop::WorkerThread 1--" << std::endl;
    while (!stop_) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return !taskQueue_.empty() || stop_;
            });
            //std::cout << "EventLoop::WorkerThread wake up." << stop_ << ", " << taskQueue_.empty() << std::endl;
            if (stop_) {
                while (!taskQueue_.empty()) {
                    task = std::move(taskQueue_.front());
                    taskQueue_.pop();
                }
                return;
            }
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        //std::cout << "EventLoop::WorkerThread exec task." << std::endl;
        task();
    }
    std::cout << "EventLoop::WorkerThread end!" << std::endl;
}

void EventLoop::AddEvent(int fd, uint32_t events, Callback&& cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->AddEvent(fd, events);
    }
    callbacks_[fd] = std::forward<Callback>(cb);
    //std::cout << "EventLoop::AddEvent " << fd << std::endl;
}

void EventLoop::ModifyEvent(int fd, uint32_t events, Callback&& cb)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->ModifyEvent(fd, events);
    }
    callbacks_[fd] = std::forward<Callback>(cb);
    //std::cout << "EventLoop::ModifyEvent " << fd << std::endl;
}

void EventLoop::RemoveEvent(int fd)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(poller_) {
        poller_->RemoveEvent(fd);
    }
    callbacks_.erase(fd);
    //std::cout << "EventLoop::RemoveEvent " << fd << std::endl;
}
