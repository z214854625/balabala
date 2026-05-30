#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor系统，管理Actor生命周期、消息路由、工作线程池

改进记录（P0/P1/P2/P3 优化）：
  [P0] 异常保护：workerLoop 中 try-catch 包裹 ProcessOne，防止 worker 线程异常退出
  [P0] 定时器系统：集成 TimerManager，支持 SetTimeout/SetInterval/CancelTimer
  [P1] Actor命名：RegisterName/FindActor/SendByName，支持按名字查找和发送消息
  [P1] 邮箱容量控制：高水位告警（在 Actor::PushMessage 中实现）
  [P1] 优雅关停：Stop() 停止定时器→停止Worker→排空邮箱→清理协程
  [P2] Actor监控/Link：LinkActor/UnlinkActor + 周期性健康检查 StartMonitor
  [P2] A.8 消息类型系统：ActorMessage 支持 std::any payload 类型安全传递
  [P2] A.9 指标监控：ActorMetrics + ActorStat 扩展，支持耗时/消息计数统计
  [P3] A.10 优先级消息：SendPriority 高优先级消息发送
  [P3] A.11 跨进程集群：RegisterTransport/SendToRemote/GetActorName，名字寻址
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
class IClusterTransport;

// [P2] A.9 Actor 统计信息（含指标数据，用于监控报告）
struct ActorStat {
    uint32_t actorId = 0;
    std::string name;                   // 命名（未命名则为空）
    size_t mailboxSize = 0;             // 当前邮箱积压量
    bool scheduled = false;             // 是否在就绪队列中
    // 指标数据
    uint64_t totalMsgProcessed = 0;     // 累计处理消息数
    uint64_t totalProcessTimeUs = 0;    // 累计处理耗时（微秒）
    uint64_t maxProcessTimeUs = 0;      // 单条消息最大处理耗时
    uint64_t avgProcessTimeUs = 0;      // 平均处理耗时
    uint64_t maxMailboxSize = 0;        // 历史最大邮箱大小
    uint64_t totalPriorityMsgProcessed = 0; // 累计优先级消息数
};

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
    // 注销Actor（会触发 Link 通知，向所有 watcher 发送 ActorDown 消息）
    void UnregisterActor(uint32_t actorId);
    // 发消息给Actor
    void Send(uint32_t actorId, ActorMessage&& msg);
    // [P3] A.10 发送高优先级消息给Actor
    void SendPriority(uint32_t actorId, ActorMessage&& msg);
    // 获取Actor
    Actor* GetActor(uint32_t actorId);
    // 获取EventLoop
    EventLoop* GetEventLoop() { return loop_; }
    // 获取当前注册的Actor数量
    size_t GetActorCount() const;

    // ===== fd到actorId的映射管理 =====
    void BindFdToActor(int fd, uint32_t actorId);
    // [2026.5 多 Reactor] 同时绑定 fd 所属的 EventLoop。SubReactor 路径调用。
    // 旧路径未传 loop 时回退使用主 loop_
    void BindFdToActor(int fd, uint32_t actorId, EventLoop* loop);
    void UnbindFd(int fd);
    uint32_t GetActorIdByFd(int fd);
    // [2026.5 多 Reactor] 查 fd 所属的 EventLoop（未绑定则返回主 loop_，兼容单 Reactor）
    EventLoop* GetEventLoopByFd(int fd);
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
    // 按actorId反查名字（未注册名字返回空串）
    std::string GetActorName(uint32_t actorId);

    // ===== [P3] A.11 跨进程集群 =====
    // 注册集群传输层（可注册多个，按 nodeId 路由）
    void RegisterTransport(IClusterTransport* transport);
    // 跨进程发送消息（按远程节点ID + Actor名字寻址）
    // senderName: 发送方 Actor 名字（用于回程路由，可选，不填则自动查找 sourceId 对应名字）
    bool SendToRemote(const std::string& targetNodeId, const std::string& targetActorName,
                      ActorMessage&& msg, const std::string& senderName = "");

    // ===== [P2] Actor监控/Link =====
    // Link: watcherId 监控 targetId，当 target 退出/注销时，watcher 收到 ActorDown 消息
    void LinkActor(uint32_t watcherId, uint32_t targetId);
    // Unlink: 取消监控
    void UnlinkActor(uint32_t watcherId, uint32_t targetId);
    // 启动周期性健康监控
    uint64_t StartMonitor(uint32_t reportActorId, int intervalMs);
    // 收集所有Actor的统计快照（线程安全，含指标数据）
    std::vector<ActorStat> CollectActorStats();

private:
    void workerLoop();
    // Link 内部：当 targetId 退出时，通知所有 watcher
    void notifyActorDown(uint32_t targetId, const std::string& reason);

    EventLoop* loop_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> nextActorId_{1};

    // Actor存储
    bllsll::SpinLock actorsLock_;
    std::unordered_map<uint32_t, std::unique_ptr<Actor>> actors_;

    // fd -> actorId映射
    bllsll::SpinLock fdMapLock_;
    std::unordered_map<int, uint32_t> fdToActor_;
    // [2026.5 多 Reactor] fd -> EventLoop 映射（用于 SubReactor 模式下按 fd 找 loop）
    std::unordered_map<int, EventLoop*> fdToLoop_;

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

    // [P2] Link 注册表: targetId → set<watcherId>
    bllsll::SpinLock linkLock_;
    std::unordered_map<uint32_t, std::set<uint32_t>> linkMap_;

    // [P3] A.11 集群传输层列表
    bllsll::SpinLock transportLock_;
    std::vector<IClusterTransport*> transports_;
};

} // namespace bllsll
