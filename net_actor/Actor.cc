/**
 * @file Actor.cc
 * @brief Actor 实现（已合并原 CoroutineActor 功能）
 *
 * 包含：
 *   - 邮箱管理（PushMessage / ProcessOne / IsMailboxEmpty）
 *   - 消息分发（OnMessage：Response → 恢复协程，新消息 → 创建协程）
 *   - Actor 间通信（SendToActor / RespondToCall / SendToNetwork）
 *   - 协程管理（Call / ClusterCall / Sleep / StoreWaiting / ResumeWaiting）
 *   - Awaiter 实现（CallAwaiter / ClusterCallAwaiter / SleepAwaiter）
 *   - 定时器 / 命名 / Link / 集群辅助方法
 */

#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"

using namespace bllsll;

// ================================================================
//  析构函数 — 清理所有未完成的协程帧
// ================================================================

Actor::~Actor()
{
    DestroyAllCoroutines();
}

// ================================================================
//  邮箱管理
// ================================================================

void Actor::PushMessage(ActorMessage&& msg)
{
    // [P3] A.10 根据 priority 字段路由到对应队列
    if (msg.priority) {
        priorityMailbox_.push(std::move(msg));
    } else {
        normalMailbox_.push(std::move(msg));
    }

    // [P1] 邮箱高水位告警
    if (mailboxHighWaterMark_ > 0) {
        size_t sz = GetMailboxSize();
        if (sz > mailboxHighWaterMark_) {
            std::cerr << "[WARN] Actor " << actorId_ << " mailbox high water mark exceeded: "
                      << sz << " > " << mailboxHighWaterMark_ << std::endl;
        }
    }

    // [P2] A.9 更新历史最大邮箱大小
    size_t currentSize = GetMailboxSize();
    uint64_t prevMax = metrics_.maxMailboxSize.load();
    while (currentSize > prevMax) {
        if (metrics_.maxMailboxSize.compare_exchange_weak(prevMax, currentSize)) {
            break;
        }
    }
}

bool Actor::ProcessOne()
{
    // [P3] A.10 优先级队列优先处理
    auto opt = priorityMailbox_.pop();
    bool isPriority = opt.has_value();
    if (!opt) {
        opt = normalMailbox_.pop();
    }
    if (!opt) {
        return false;
    }

    // [P2] A.9 指标统计：计时
    auto start = std::chrono::steady_clock::now();
    OnMessage(*opt);
    auto elapsed = std::chrono::steady_clock::now() - start;
    uint64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    metrics_.totalMsgProcessed.fetch_add(1);
    metrics_.totalProcessTimeUs.fetch_add(elapsedUs);
    if (isPriority) {
        metrics_.totalPriorityMsgProcessed.fetch_add(1);
    }

    // 更新单条消息最大处理耗时
    uint64_t prevMax = metrics_.maxProcessTimeUs.load();
    while (elapsedUs > prevMax) {
        if (metrics_.maxProcessTimeUs.compare_exchange_weak(prevMax, elapsedUs)) {
            break;
        }
    }

    return true;
}

bool Actor::IsMailboxEmpty() const
{
    return priorityMailbox_.empty() && normalMailbox_.empty();
}

size_t Actor::GetMailboxSize() const
{
    return priorityMailbox_.size() + normalMailbox_.size();
}

// ================================================================
//  消息分发（合并自 CoroutineActor::OnMessage）
// ================================================================

void Actor::OnMessage(ActorMessage& msg)
{
    if (msg.isResponse && msg.sessionId > 0) {
        // 这是一个 Response 消息 — 恢复等待的协程
        if (!ResumeWaiting(msg.sessionId, std::move(msg))) {
            // 常见原因：超时已先触发并恢复了协程，此后真响应才姗姗来迟 → 安全丢弃
            std::cerr << "[Actor:" << actorId_ << "] late/orphan response dropped, session=" << msg.sessionId << std::endl;
        }
    } else {
        // 新消息 — 创建新协程处理
        // 参数按值传递，因为协程可能挂起，原引用会在 ProcessOne() 返回后失效
        OnCoroutineMessage(std::move(msg));
    }
}

// ================================================================
//  Skynet 风格协程 API
// ================================================================

CallAwaiter Actor::Call(uint32_t targetId, ActorMessage&& msg, int timeoutMs)
{
    return CallAwaiter{this, targetId, std::move(msg), timeoutMs};
}

ClusterCallAwaiter Actor::ClusterCall(const std::string& targetNodeId,
                                       const std::string& targetActorName,
                                       ActorMessage&& msg,
                                       int timeoutMs)
{
    return ClusterCallAwaiter{this, targetNodeId, targetActorName, std::move(msg), timeoutMs};
}

void Actor::Respond(const ActorMessage& request, ActorMessage&& response)
{
    // 委托给 RespondToCall
    RespondToCall(request, std::move(response));
}

SleepAwaiter Actor::Sleep(int ms)
{
    return SleepAwaiter{this, ms};
}

// ================================================================
//  协程管理
// ================================================================

void Actor::StoreWaiting(uint32_t session, std::coroutine_handle<> h)
{
    waitMap_[session] = h;
}

bool Actor::ResumeWaiting(uint32_t session, ActorMessage&& response)
{
    auto it = waitMap_.find(session);
    if (it == waitMap_.end()) {
        return false;
    }
    auto h = it->second;
    waitMap_.erase(it);

    // 先存储响应，再 resume，这样 await_resume() 可以取到
    responseMap_[session] = std::move(response);
    h.resume();  // 协程从 co_await 处继续执行

    return true;
}

void Actor::DestroyAllCoroutines()
{
    for (auto& [session, h] : waitMap_) {
        if (h && !h.done()) {
            std::cout << "[Actor:" << actorId_
                      << "] destroying pending coroutine session=" << session << std::endl;
            h.destroy();
        }
    }
    waitMap_.clear();
    responseMap_.clear();
}

// ================================================================
//  Actor 间通信
// ================================================================

void Actor::SendToActor(uint32_t targetId, ActorMessage&& msg)
{
    if (system_) {
        msg.sourceId = actorId_;
        system_->Send(targetId, std::move(msg));
    }
}

void Actor::SendPriorityToActor(uint32_t targetId, ActorMessage&& msg)
{
    if (system_) {
        msg.sourceId = actorId_;
        msg.priority = true;
        system_->Send(targetId, std::move(msg));
    }
}

void Actor::RespondToCall(const ActorMessage& request, ActorMessage&& response)
{
    if (system_) {
        response.sourceId = actorId_;
        response.sessionId = request.sessionId;
        response.isResponse = true;
        system_->Send(request.sourceId, std::move(response));
    }
}

void Actor::SendToNetwork(int fd, const char* pData, int nLen)
{
    if (system_) {
        // [2026.5 多 Reactor] 按 fd 查找对应 EventLoop（SubReactor 模式下 fd 分布在多个 loop）
        // 兼容：单 Reactor 时 GetEventLoopByFd 回退返回主 loop_
        auto* loop = system_->GetEventLoopByFd(fd);
        if (loop) {
            auto* pConn = loop->GetConnection(fd);
            if (pConn) {
                pConn->Send(pData, nLen);
            } else {
                std::cout << "Actor::SendToNetwork pConn null! fd=" << fd << std::endl;
            }
        }
    }
}

// ================================================================
//  [P0] 定时器辅助方法（委托给 ActorSystem）
// ================================================================

uint64_t Actor::SetTimeout(int delayMs, ActorMessage&& msg)
{
    if (system_) {
        return system_->SetTimeout(actorId_, delayMs, std::move(msg));
    }
    return 0;
}

uint64_t Actor::SetInterval(int intervalMs, ActorMessage&& msg)
{
    if (system_) {
        return system_->SetInterval(actorId_, intervalMs, std::move(msg));
    }
    return 0;
}

void Actor::CancelTimer(uint64_t timerId)
{
    if (system_) {
        system_->CancelTimer(timerId);
    }
}

// ================================================================
//  [P1] 命名辅助方法（委托给 ActorSystem）
// ================================================================

uint32_t Actor::FindActorByName(const std::string& name)
{
    if (system_) {
        return system_->FindActor(name);
    }
    return 0;
}

bool Actor::SendByName(const std::string& name, ActorMessage&& msg)
{
    if (system_) {
        msg.sourceId = actorId_;
        return system_->SendByName(name, std::move(msg));
    }
    return false;
}

// ================================================================
//  [P2] 监控/Link 辅助方法（委托给 ActorSystem）
// ================================================================

void Actor::LinkTo(uint32_t targetActorId)
{
    if (system_) {
        system_->LinkActor(actorId_, targetActorId);
    }
}

void Actor::UnlinkFrom(uint32_t targetActorId)
{
    if (system_) {
        system_->UnlinkActor(actorId_, targetActorId);
    }
}

// ================================================================
//  [P3] A.11 跨进程集群辅助方法
// ================================================================

bool Actor::SendToRemote(const std::string& targetNodeId, const std::string& targetActorName,
                          ActorMessage&& msg)
{
    if (!system_) return false;
    msg.sourceId = actorId_;
    // senderName 由 ActorSystem::SendToRemote 内部通过 GetActorName 自动查找
    return system_->SendToRemote(targetNodeId, targetActorName, std::move(msg));
}

bool Actor::RespondRemote(const ActorMessage& request, const std::string& responseData)
{
    if (!request.IsRemote() || !system_) return false;
    ActorMessage resp{MsgType::UserMessage, actorId_, -1, responseData};
    resp.sessionId = request.sessionId;
    resp.isResponse = true;
    return system_->SendToRemote(request.sourceNodeId, request.sourceActorName,
                                  std::move(resp));
}

// ================================================================
//  CallAwaiter 实现
// ================================================================

void CallAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 1. 分配 sessionId
    sessionId = actor->AllocSession();

    // 2. 存储协程句柄（等待恢复）
    actor->StoreWaiting(sessionId, h);

    // 3. 设置消息的 sessionId，发送给目标 Actor
    msg.sessionId = sessionId;
    actor->SendToActor(targetId, std::move(msg));

    // 4. 注册超时定时器（timeoutMs <= 0 表示永久等待）
    if (timeoutMs > 0) {
        auto* sys = actor->GetSystem();
        if (sys) {
            ActorMessage timeoutMsg{MsgType::UserMessage, 0, -1, ""};
            timeoutMsg.sessionId = sessionId;
            timeoutMsg.isResponse = true;
            timeoutMsg.error = CallError::Timeout;
            timerId = sys->SetTimeout(actor->GetActorId(), timeoutMs,
                                       std::move(timeoutMsg));
        }
    }

    // 5. 返回后协程挂起，worker 线程释放
}

ActorMessage CallAwaiter::await_resume()
{
    // 从 responseMap_ 取出响应消息
    auto it = actor->responseMap_.find(sessionId);
    ActorMessage result;
    if (it != actor->responseMap_.end()) {
        result = std::move(it->second);
        actor->responseMap_.erase(it);
    }

    // 若是正常响应（无错误），取消尚未触发的超时定时器，避免邮箱噪音
    if (result.error == CallError::Ok && timerId != 0) {
        auto* sys = actor->GetSystem();
        if (sys) sys->CancelTimer(timerId);
    }

    return result;
}

// ================================================================
//  ClusterCallAwaiter 实现 — 跨进程协程 RPC
// ================================================================

void ClusterCallAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 1. 分配 sessionId
    sessionId = actor->AllocSession();

    // 2. 存储协程句柄（等待恢复）
    actor->StoreWaiting(sessionId, h);

    // 3. 设置消息的 sessionId，通过 SendToRemote 发送到远端节点
    msg.sessionId = sessionId;
    actor->SendToRemote(targetNodeId, targetActorName, std::move(msg));

    // 4. 注册超时定时器（跨进程网络更易丢/慢，超时机制尤为重要）
    if (timeoutMs > 0) {
        auto* sys = actor->GetSystem();
        if (sys) {
            ActorMessage timeoutMsg{MsgType::UserMessage, 0, -1, ""};
            timeoutMsg.sessionId = sessionId;
            timeoutMsg.isResponse = true;
            timeoutMsg.error = CallError::Timeout;
            timerId = sys->SetTimeout(actor->GetActorId(), timeoutMs,
                                       std::move(timeoutMsg));
        }
    }

    // 5. 返回后协程挂起，worker 线程释放
}

ActorMessage ClusterCallAwaiter::await_resume()
{
    // 从 responseMap_ 取出响应消息（与 CallAwaiter::await_resume 完全相同）
    auto it = actor->responseMap_.find(sessionId);
    ActorMessage result;
    if (it != actor->responseMap_.end()) {
        result = std::move(it->second);
        actor->responseMap_.erase(it);
    }

    // 若是正常响应（无错误），取消尚未触发的超时定时器
    if (result.error == CallError::Ok && timerId != 0) {
        auto* sys = actor->GetSystem();
        if (sys) sys->CancelTimer(timerId);
    }

    return result;
}

// ================================================================
//  SleepAwaiter 实现
//  [P0] 改用 TimerManager（ActorSystem::SetTimeout），
//       不再启动 detached thread
// ================================================================

void SleepAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 1. 分配 sessionId
    sessionId = actor->AllocSession();

    // 2. 存储协程句柄
    actor->StoreWaiting(sessionId, h);

    // 3. 通过 TimerManager 注册一次性定时器
    //    定时器到期后会发送一个 isResponse=true 的消息，触发协程恢复
    auto* sys = actor->GetSystem();
    if (sys) {
        ActorMessage wakeup{MsgType::UserMessage, 0, -1, ""};
        wakeup.sessionId = sessionId;
        wakeup.isResponse = true;
        sys->SetTimeout(actor->GetActorId(), milliseconds, std::move(wakeup));
    }

    // 4. 返回后协程挂起
}
