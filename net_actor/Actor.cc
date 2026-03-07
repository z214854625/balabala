#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"

using namespace bllsll;

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
        auto* loop = system_->GetEventLoop();
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
