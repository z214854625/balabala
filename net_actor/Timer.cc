#include "Timer.h"
#include "ActorSystem.h"

using namespace bllsll;

TimerManager::~TimerManager()
{
    Stop();
}

void TimerManager::Start(ActorSystem* sys)
{
    system_ = sys;
    running_ = true;
    timerThread_ = std::thread(&TimerManager::timerLoop, this);
    std::cout << "TimerManager started." << std::endl;
}

void TimerManager::Stop()
{
    if (!running_.exchange(false)) {
        return;  // 已经停止
    }
    timerCv_.notify_all();
    if (timerThread_.joinable()) {
        timerThread_.join();
    }
    // 清理所有定时器
    std::lock_guard<std::mutex> lock(timerMutex_);
    while (!heap_.empty()) {
        heap_.pop();
    }
    timerMap_.clear();
    std::cout << "TimerManager stopped." << std::endl;
}

uint64_t TimerManager::SetTimeout(uint32_t actorId, int delayMs, ActorMessage&& msg)
{
    return addTimer(actorId, delayMs, std::move(msg), 0);
}

uint64_t TimerManager::SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg)
{
    if (intervalMs <= 0) {
        std::cout << "TimerManager::SetInterval invalid intervalMs=" << intervalMs << std::endl;
        return 0;
    }
    return addTimer(actorId, intervalMs, std::move(msg), intervalMs);
}

void TimerManager::CancelTimer(uint64_t timerId)
{
    std::lock_guard<std::mutex> lock(timerMutex_);
    auto it = timerMap_.find(timerId);
    if (it != timerMap_.end()) {
        it->second->cancelled = true;  // 惰性删除标记
        timerMap_.erase(it);
    }
}

uint64_t TimerManager::NowMs()
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
}

size_t TimerManager::GetActiveTimerCount() const
{
    std::lock_guard<std::mutex> lock(timerMutex_);
    return timerMap_.size();
}

uint64_t TimerManager::addTimer(uint32_t actorId, int delayMs, ActorMessage&& msg, int intervalMs)
{
    uint64_t timerId = nextTimerId_.fetch_add(1);
    auto entry = std::make_shared<TimerEntry>();
    entry->timerId = timerId;
    entry->expireTimeMs = NowMs() + delayMs;
    entry->actorId = actorId;
    entry->msg = std::move(msg);
    entry->intervalMs = intervalMs;
    entry->cancelled = false;

    {
        std::lock_guard<std::mutex> lock(timerMutex_);
        timerMap_[timerId] = entry;
        heap_.push(entry);
    }
    timerCv_.notify_one();  // 唤醒定时器线程检查新的最近到期时间
    return timerId;
}

void TimerManager::timerLoop()
{
    while (running_) {
        // 收集已到期的定时器（在锁内），然后在锁外发送消息（避免死锁）
        std::vector<std::pair<uint32_t, ActorMessage>> toFire;

        {
            std::unique_lock<std::mutex> lock(timerMutex_);

            if (heap_.empty()) {
                // 无定时器，等待新定时器加入或停止信号
                timerCv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this]{ return !running_ || !heap_.empty(); });
                continue;
            }

            uint64_t now = NowMs();
            uint64_t nextExpire = heap_.top()->expireTimeMs;

            if (nextExpire > now) {
                // 等待到最近的定时器过期，或新定时器加入
                timerCv_.wait_for(lock, std::chrono::milliseconds(nextExpire - now),
                    [this, nextExpire]{
                        return !running_ ||
                               (!heap_.empty() && heap_.top()->expireTimeMs <= NowMs());
                    });
                if (!running_) break;
                continue;  // 重新检查（可能有更早的定时器被添加）
            }

            // 批量收集所有已到期的定时器
            while (!heap_.empty() && heap_.top()->expireTimeMs <= NowMs()) {
                auto entry = heap_.top();
                heap_.pop();

                if (entry->cancelled) {
                    continue;  // 已取消，跳过
                }

                // 拷贝消息用于发送（原消息可能被周期定时器复用）
                toFire.emplace_back(entry->actorId, entry->msg);

                if (entry->intervalMs > 0) {
                    // 周期性定时器：更新过期时间，重新入堆
                    entry->expireTimeMs = NowMs() + entry->intervalMs;
                    heap_.push(entry);
                } else {
                    // 一次性定时器：从查找表移除
                    timerMap_.erase(entry->timerId);
                }
            }
        }

        // 在锁外发送消息（避免与 ActorSystem::Send 的锁发生死锁）
        for (auto& [actorId, msg] : toFire) {
            if (system_ && running_) {
                system_->Send(actorId, std::move(msg));
            }
        }
    }
}
