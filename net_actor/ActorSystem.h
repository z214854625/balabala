#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor系统，管理Actor生命周期、消息路由、工作线程池

改进记录（P0/P1 优化）：
  [P0] 异常保护：workerLoop 中 try-catch 包裹 ProcessOne，防止 worker 线程异常退出
  [P0] 定时器系统：集成 TimerManager，支持 SetTimeout/SetInterval/CancelTimer
  [P1] Actor命名：RegisterName/FindActor/SendByName，支持按名字查找和发送消息
  [P1] 邮箱容量控制：高水位告警（在 Actor::PushMessage 中实现）
  [P1] 优雅关停：Stop() 停止定时器→停止Worker→排空邮箱→清理协程
*/

#include "precompiled.h"
#include "Actor.h"
#include "Message.h"
#include "Timer.h"
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
    // 停止Actor系统（优雅关停：定时器→Worker→排空邮箱→清理协程）
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

    // ===== fd到actorId的映射管理 =====
    void BindFdToActor(int fd, uint32_t actorId);
    void UnbindFd(int fd);
    uint32_t GetActorIdByFd(int fd);
    // 通过fd发送消息（内部查找绑定的actorId）
    void SendByFd(int fd, ActorMessage&& msg);

    // ===== [P0] 定时器管理 =====
    // 一次性定时器：delayMs 毫秒后给 actorId 发送 msg
    uint64_t SetTimeout(uint32_t actorId, int delayMs, ActorMessage&& msg);
    // 周期性定时器：每隔 intervalMs 毫秒给 actorId 发送 msg
    uint64_t SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg);
    // 取消定时器
    void CancelTimer(uint64_t timerId);
    // 获取TimerManager
    TimerManager* GetTimerManager() { return &timerManager_; }

    // ===== [P1] Actor命名/发现 =====
    // 按名字注册Actor
    void RegisterName(const std::string& name, uint32_t actorId);
    // 取消名字注册
    void UnregisterName(const std::string& name);
    // 按名字查找Actor ID（未找到返回0）
    uint32_t FindActor(const std::string& name);
    // 按名字发消息
    bool SendByName(const std::string& name, ActorMessage&& msg);

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

    // [P0] 定时器管理器
    TimerManager timerManager_;

    // [P1] Actor名字 → actorId 映射
    bllsll::SpinLock nameLock_;
    std::unordered_map<std::string, uint32_t> nameToActor_;
    std::unordered_map<uint32_t, std::string> actorToName_;  // 反向映射，用于注销时清理
};

} // namespace bllsll
