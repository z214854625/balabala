#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: I/O事件循环处理
*/

#include "precompiled.h"
#include "../Util/SpinLockQueue.h"

namespace sll {

class Poller;

class EventLoop
{
public:
    using Functor = std::function<void()>;
    using Callback = std::function<void(int, uint32_t)>;

    EventLoop();
    ~EventLoop();
    //创建
    void Create(int);
    //事件循环
    void run(int timeout = -1);
    //线程函数
    void WorkerThread();
    //添加io事件
    void AddEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件
    void ModifyEvent(int fd, uint32_t events, Callback&& cb);
    //删除io事件
    void RemoveEvent(int fd);
    //获取poller对象
    Poller* GetPoller() { return poller_.get(); };
    //派发任务
    void OnDispatch(int timeout = 0);
    //获取系统毫秒
    int64_t GetMilliSeconds();
private:
    std::unique_ptr<Poller> poller_;
    bool stop_;
    std::vector<std::thread> threadPool_;
    sll::SpinLockQueue<std::function<void()>> taskQueue_;
    sll::SpinLock spinLock_;
    std::unordered_map<int, Callback> callbacks_;
};

} //namespace sll