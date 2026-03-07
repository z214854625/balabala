#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor基类，每个Actor拥有独立的邮箱(mailbox)，消息串行处理

改进记录（P0/P1/P2 优化）：
  [P0] 定时器辅助：SetTimeout/SetInterval/CancelTimer（委托给ActorSystem）
  [P1] 命名辅助：FindActorByName/SendByName（委托给ActorSystem）
  [P1] 邮箱高水位：SetMailboxHighWaterMark()，超过水位时打印告警
  [P2] 监控/Link辅助：LinkTo/UnlinkFrom，被监控Actor退出时收到ActorDown消息
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

    // 往邮箱投递消息（[P1] 内含高水位告警）
    void PushMessage(ActorMessage&& msg);
    // 从邮箱取一条消息处理，返回是否处理了消息
    bool ProcessOne();
    // 邮箱是否为空
    bool IsMailboxEmpty() const;
    // [P1] 获取当前邮箱大小
    size_t GetMailboxSize() const;

    // 调度标记（用于ActorSystem的就绪队列去重）
    std::atomic<bool> scheduled_{false};

    // 子类实现：处理消息
    virtual void OnMessage(ActorMessage& msg) = 0;

    // [P1] 设置邮箱高水位告警阈值（0=禁用，默认禁用）
    void SetMailboxHighWaterMark(size_t hwm) { mailboxHighWaterMark_ = hwm; }

protected:
    // 发消息给其他Actor
    void SendToActor(uint32_t targetId, ActorMessage&& msg);
    // 响应协程Call请求（将response的sessionId/isResponse自动填充后发回给调用者）
    void RespondToCall(const ActorMessage& request, ActorMessage&& response);
    // 发送网络数据（通过EventLoop的Connection::Send）
    void SendToNetwork(int fd, const char* pData, int nLen);

    // ===== [P0] 定时器辅助方法 =====
    // 一次性定时器：delayMs 毫秒后给自己发送 msg
    uint64_t SetTimeout(int delayMs, ActorMessage&& msg);
    // 周期性定时器：每隔 intervalMs 毫秒给自己发送 msg
    uint64_t SetInterval(int intervalMs, ActorMessage&& msg);
    // 取消定时器
    void CancelTimer(uint64_t timerId);

    // ===== [P1] 命名辅助方法 =====
    // 按名字查找Actor ID
    uint32_t FindActorByName(const std::string& name);
    // 按名字发送消息
    bool SendByName(const std::string& name, ActorMessage&& msg);

    // ===== [P2] 监控/Link 辅助方法 =====
    // 监控目标Actor（target退出时本Actor收到 ActorDown 消息）
    void LinkTo(uint32_t targetActorId);
    // 取消监控
    void UnlinkFrom(uint32_t targetActorId);

    uint32_t actorId_ = 0;
    ActorSystem* system_ = nullptr;
    bllsll::SpinLockQueue<ActorMessage> mailbox_;
    size_t mailboxHighWaterMark_ = 0;  // [P1] 高水位告警阈值（0=禁用）
};

} // namespace bllsll
