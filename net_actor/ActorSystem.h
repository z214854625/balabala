#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor系统，管理Actor生命周期、消息路由、工作线程池
*/

#include "precompiled.h"
#include "Actor.h"
#include "Message.h"
#include "../Util/SpinLock.h"
#include "../Util/LockGuard.h"
#include "../Util/SpinLockQueue.h"

namespace bllsll {

class EventLoop;

class ActorSystem
{
public:
    ActorSystem();
    ~ActorSystem();

    // 启动Actor系统（workerCount个工作线程）
    void Start(int workerCount, EventLoop* loop);
    // 停止Actor系统
    void Stop();

    // 注册Actor，返回actorId
    uint32_t RegisterActor(std::unique_ptr<Actor> actor);
    // 注销Actor
    void UnregisterActor(uint32_t actorId);
    // 发消息给Actor
    void Send(uint32_t actorId, ActorMessage&& msg);
    // 获取Actor
    Actor* GetActor(uint32_t actorId);
    // 获取EventLoop
    EventLoop* GetEventLoop() { return loop_; }

    // fd到actorId的映射管理
    void BindFdToActor(int fd, uint32_t actorId);
    void UnbindFd(int fd);
    uint32_t GetActorIdByFd(int fd);
    // 通过fd发送消息（内部查找绑定的actorId）
    void SendByFd(int fd, ActorMessage&& msg);

private:
    void workerLoop();

    EventLoop* loop_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> nextActorId_{1};

    // Actor存储
    bllsll::SpinLock actorsLock_;
    std::unordered_map<uint32_t, std::unique_ptr<Actor>> actors_;

    // fd -> actorId映射
    bllsll::SpinLock fdMapLock_;
    std::unordered_map<int, uint32_t> fdToActor_;

    // 工作线程
    std::vector<std::thread> workers_;

    // 就绪队列（有消息待处理的actor id）
    bllsll::SpinLockQueue<uint32_t> readyQueue_;

    // 条件变量用于唤醒工作线程
    std::mutex cvMutex_;
    std::condition_variable cv_;
};

} // namespace bllsll
