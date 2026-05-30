#pragma once
/**
@auther: AI重写
@date: 2026.5
@brief: EventLoop 线程池 — 主从 Reactor 架构的核心
       主 Reactor（MainReactor）负责 Acceptor 接收新连接，
       从 Reactor（SubReactor）负责处理已建立连接的 I/O 读写。
       新连接到达时，Round-Robin 派发到某个 SubReactor 上。

使用示例：
  EventLoopThreadPool pool;
  pool.SetThreadNum(4);          // 4 个 SubReactor
  pool.Start();                   // 启动所有 SubReactor 线程
  EventLoop* sub = pool.GetNextLoop();  // Round-Robin 取下一个

兼容性：
  - SetThreadNum(0) 时不创建任何线程，GetNextLoop() 返回 nullptr，
    调用方应回退到主 loop（保留单 Reactor 行为）
*/

#include "EventLoop.h"
#include <vector>
#include <memory>
#include <atomic>

namespace bllsll {

class ActorSystem;

class EventLoopThreadPool
{
public:
    EventLoopThreadPool() = default;
    ~EventLoopThreadPool() { Stop(); }

    // 设置 SubReactor 数量。0 表示不启用（保持单 Reactor 行为）
    void SetThreadNum(int n) { threadNum_ = n; }

    // 启动所有 SubReactor 线程。可关联同一个 ActorSystem 让所有 sub loop 共享 Actor 池
    void Start(ActorSystem* sys = nullptr)
    {
        if (started_) return;
        started_ = true;
        for (int i = 0; i < threadNum_; ++i) {
            auto loop = std::make_unique<EventLoop>();
            loop->Create();
            if (sys) loop->SetActorSystem(sys);
            loops_.emplace_back(std::move(loop));
        }
        std::cout << "EventLoopThreadPool started with " << threadNum_
                  << " sub reactors." << std::endl;
    }

    // 停止并清理所有 SubReactor 线程
    void Stop()
    {
        if (!started_) return;
        started_ = false;
        for (auto& loop : loops_) {
            if (loop) loop->Stop();
        }
        loops_.clear();
    }

    // Round-Robin 取下一个 SubReactor 用于派发新连接。
    // 无 SubReactor 时返回 nullptr，调用方应回退到主 loop
    EventLoop* GetNextLoop()
    {
        if (loops_.empty()) return nullptr;
        size_t idx = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
        return loops_[idx].get();
    }

    // 是否启用了 SubReactor
    bool Enabled() const { return !loops_.empty(); }

    // 获取所有 sub loop 列表（用于上层遍历，比如统计/优雅关停）
    const std::vector<std::unique_ptr<EventLoop>>& GetAllLoops() const { return loops_; }

private:
    int threadNum_ = 0;
    bool started_ = false;
    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::atomic<size_t> next_{0};
};

} // namespace bllsll
