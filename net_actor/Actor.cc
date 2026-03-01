#include "Actor.h"
#include "ActorSystem.h"
#include "EventLoop.h"

using namespace bllsll;

void Actor::PushMessage(ActorMessage&& msg)
{
    mailbox_.push(std::move(msg));
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

void Actor::SendToActor(uint32_t targetId, ActorMessage&& msg)
{
    if (system_) {
        msg.sourceId = actorId_;
        system_->Send(targetId, std::move(msg));
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
