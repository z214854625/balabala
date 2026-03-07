#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: 定时器管理器 — 基于最小堆的高效定时器

设计思路：
  使用单独的定时器线程 + 最小堆(priority_queue)实现：
  - SetTimeout: 一次性定时器，到期后发送消息给目标Actor
  - SetInterval: 周期性定时器，每隔指定时间发送消息
  - CancelTimer: 取消定时器（惰性删除，标记cancelled后在堆弹出时跳过）

  替代原来 SleepAwaiter 中每次创建 std::thread 的做法，
  所有定时器共享一个线程，性能大幅提升。

  内存管理：使用 shared_ptr 管理 TimerEntry 生命周期，
  heap_ 和 timerMap_ 同时持有引用，取消时从 timerMap_ 移除并标记 cancelled，
  堆弹出时自动释放。

使用示例：
  // 一次性定时器：500ms 后给 actorId 发消息
  auto timerId = system->SetTimeout(actorId, 500,
      ActorMessage{MsgType::UserMessage, 0, -1, "timeout_event"});

  // 周期性定时器：每 50ms 给 actorId 发一次 tick
  auto tickId = system->SetInterval(actorId, 50,
      ActorMessage{MsgType::UserMessage, 0, -1, "tick"});

  // 取消定时器
  system->CancelTimer(tickId);
*/

#include "precompiled.h"
#include "Message.h"

namespace bllsll {

class ActorSystem;

// 定时器条目
struct TimerEntry {
    uint64_t timerId = 0;
    uint64_t expireTimeMs = 0;    // 绝对过期时间（毫秒）
    uint32_t actorId = 0;
    ActorMessage msg;
    int intervalMs = 0;           // 0=一次性, >0=周期性
    bool cancelled = false;
};

using TimerEntryPtr = std::shared_ptr<TimerEntry>;

// 最小堆比较器（过期时间最小的在顶部）
struct TimerCompare {
    bool operator()(const TimerEntryPtr& a, const TimerEntryPtr& b) const {
        return a->expireTimeMs > b->expireTimeMs;
    }
};

class TimerManager
{
public:
    TimerManager() = default;
    ~TimerManager();

    // 启动定时器线程
    void Start(ActorSystem* sys);
    // 停止定时器线程
    void Stop();

    // 一次性定时器：delayMs 毫秒后给 actorId 发送 msg
    // 返回 timerId，可用于 CancelTimer
    uint64_t SetTimeout(uint32_t actorId, int delayMs, ActorMessage&& msg);

    // 周期性定时器：每隔 intervalMs 毫秒给 actorId 发送 msg
    // 返回 timerId，可用于 CancelTimer
    uint64_t SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg);

    // 取消定时器（惰性删除）
    void CancelTimer(uint64_t timerId);

    // 获取当前系统时间（毫秒）
    static uint64_t NowMs();

    // 获取活跃定时器数量
    size_t GetActiveTimerCount() const;

private:
    void timerLoop();
    uint64_t addTimer(uint32_t actorId, int delayMs, ActorMessage&& msg, int intervalMs);

    ActorSystem* system_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> nextTimerId_{1};
    std::thread timerThread_;

    mutable std::mutex timerMutex_;
    std::condition_variable timerCv_;
    std::priority_queue<TimerEntryPtr, std::vector<TimerEntryPtr>, TimerCompare> heap_;
    std::unordered_map<uint64_t, TimerEntryPtr> timerMap_;  // timerId → entry（用于取消查找）
};

} // namespace bllsll
