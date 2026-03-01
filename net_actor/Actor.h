#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor基类，每个Actor拥有独立的邮箱(mailbox)，消息串行处理
*/

#include "precompiled.h"
#include "Message.h"
#include "../Util/SpinLockQueue.h"

namespace bllsll {

class ActorSystem;

class Actor
{
public:
    Actor() = default;
    virtual ~Actor() = default;

    uint32_t GetActorId() const { return actorId_; }
    void SetActorId(uint32_t id) { actorId_ = id; }
    void SetSystem(ActorSystem* sys) { system_ = sys; }
    ActorSystem* GetSystem() { return system_; }

    // 往邮箱投递消息
    void PushMessage(ActorMessage&& msg);
    // 从邮箱取一条消息处理，返回是否处理了消息
    bool ProcessOne();
    // 邮箱是否为空
    bool IsMailboxEmpty() const;

    // 调度标记（用于ActorSystem的就绪队列去重）
    std::atomic<bool> scheduled_{false};

    // 子类实现：处理消息
    virtual void OnMessage(ActorMessage& msg) = 0;

protected:
    // 发消息给其他Actor
    void SendToActor(uint32_t targetId, ActorMessage&& msg);
    // 发送网络数据（通过EventLoop的Connection::Send）
    void SendToNetwork(int fd, const char* pData, int nLen);

    uint32_t actorId_ = 0;
    ActorSystem* system_ = nullptr;
    bllsll::SpinLockQueue<ActorMessage> mailbox_;
};

} // namespace bllsll
