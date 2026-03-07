#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"

using namespace bllsll;

void Actor::PushMessage(ActorMessage&& msg)
{
    mailbox_.push(std::move(msg));
    // [P1] 邮箱高水位告警
    if (mailboxHighWaterMark_ > 0) {
        size_t sz = mailbox_.size();
        if (sz > mailboxHighWaterMark_) {
            std::cerr << "[WARN] Actor " << actorId_ << " mailbox high water mark exceeded: "
                      << sz << " > " << mailboxHighWaterMark_ << std::endl;
        }
    }
}

bool Actor::ProcessOne()
{
    auto opt = mailbox_.pop();
    if (!opt) {
        return false;
    }
    OnMessage(*opt);
    return true;
}

bool Actor::IsMailboxEmpty() const
{
    return mailbox_.empty();
}

size_t Actor::GetMailboxSize() const
{
    return mailbox_.size();
}

void Actor::SendToActor(uint32_t targetId, ActorMessage&& msg)
{
    if (system_) {
        msg.sourceId = actorId_;
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
