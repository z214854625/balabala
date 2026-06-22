#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: I/O事件循环处理（Actor模型版本）
线程模型：
- 1个 epoll loop 线程：负责所有I/O操作（accept/read/write），保证同一fd的I/O串行
- Actor工作线程池：负责业务逻辑处理（由ActorSystem管理）
- I/O线程读到数据后，通过ActorSystem路由给对应的Actor
- Send() 可从任意线程调用，通过 SpinLockQueue + epoll_ctl 保证线程安全
*/

#include "precompiled.h"
#include "../Util/SpinLock.h"
#include "IConnection.h"
#include <atomic>

namespace bllsll {

class Poller;
class ActorSystem;

class EventLoop
{
public:
    using Callback = std::function<void(int, uint32_t)>;
    using PendingTask = std::function<void()>;

    EventLoop();
    ~EventLoop();
    //创建（启动epoll loop线程）
    void Create();
    //事件循环（在loop线程中直接执行I/O回调）
    void run(int timeout = -1);
    //停止 loop（线程安全；通过 eventfd 唤醒并退出）
    void Stop();
    //添加io事件（线程安全：自动路由到 loop 线程执行）
    void AddEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件（线程安全）
    void ModifyEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件，但保留原回调（线程安全；Send 路径用，避免每次都拷贝回调）
    void ModifyEventKeepCallback(int fd, uint32_t events);
    //删除io事件（线程安全）
    void RemoveEvent(int fd);
    //获取poller对象
    Poller* GetPoller() { return poller_.get(); };
    //设置ActorSystem（由TcpServer/TcpClient设置）
    void SetActorSystem(ActorSystem* actorSys) { actorSystem_ = actorSys; }
    //获取ActorSystem
    ActorSystem* GetActorSystem() { return actorSystem_; }
    //获取系统毫秒
    int64_t GetMilliSeconds();
    //加入连接对象列表（shared_ptr 版本，用于 Connection）
    void AddConnection(std::shared_ptr<bllsll::IConnection> pConn);
    //加入连接对象列表（裸指针版本，用于 Acceptor/Connector，不管理生命周期）
    void AddConnectionRaw(bllsll::IConnection* pConn);
    //获取连接对象（返回 shared_ptr，调用者持有期间不会被析构）
    std::shared_ptr<bllsll::IConnection> GetConnection(int fd);
    //删除连接对象（线程安全：自动延迟到 loop 线程执行，避免 HandleRead 自删的 UAF）
    void RemoveConnection(int fd);

    //当前是否在 loop 线程
    bool IsInLoopThread() const;
    //在 loop 线程执行任务：若已在则同步执行，否则入队 + 唤醒
    void RunInLoop(PendingTask&& task);
    //无论在哪个线程都入队 + 唤醒（loop 线程内调用可避免递归）
    void QueueInLoop(PendingTask&& task);

private:
    //唤醒 eventfd（处理写端）
    void wakeup();
    //处理 eventfd 读端
    void handleWakeup();
    //执行所有 pendingTasks_
    void runPendingTasks();

    std::unique_ptr<Poller> poller_;
    std::atomic<bool> stop_{false};
    std::thread loopThread_;                        //epoll loop线程
    std::thread::id loopThreadId_{};                 //loop线程id，用于 IsInLoopThread
    int wakeupFd_ = -1;                              //eventfd，用于跨线程唤醒/退出
    mutable bllsll::SpinLock pendingLock_;                   //保护 pendingTasks_
    std::vector<PendingTask> pendingTasks_;          //跨线程投递的任务

    mutable bllsll::SpinLock cbSpinLock_;                    //保护callbacks_
    std::unordered_map<int, Callback> callbacks_;    //fd→回调映射
    mutable bllsll::SpinLock connSpinLock_;                  //保护mapConn_
    std::unordered_map<int, std::shared_ptr<bllsll::IConnection>> mapConn_; //连接对象（shared_ptr 管理）
    std::unordered_map<int, bllsll::IConnection*> mapConnRaw_;  //裸指针连接（Acceptor/Connector，不管理生命周期）
    ActorSystem* actorSystem_ = nullptr;             //Actor系统（不拥有，由外部管理）
};

} //namespace bllsll
