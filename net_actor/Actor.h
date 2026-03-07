#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor基类，每个Actor拥有独立的邮箱(mailbox)，消息串行处理

改进记录（P0/P1/P2/P3 优化）：
 [P0] 定时器辅助：SetTimeout/SetInterval/CancelTimer（委托给ActorSystem）
 [P1] 命名辅助：FindActorByName/SendByName（委托给ActorSystem）
 [P1] 邮箱高水位：SetMailboxHighWaterMark()，超过水位时打印告警
 [P2] 监控/Link辅助：LinkTo/UnlinkFrom，被监控Actor退出时收到ActorDown消息
 [P2] A.9 指标监控：ActorMetrics 结构，ProcessOne 内自动统计耗时和消息计数
 [P3] A.10 优先级消息：双队列（priorityMailbox_ + normalMailbox_），优先处理高优先级消息
 [P3] A.11 跨进程集群：SendToRemote/RespondRemote，便捷的跨进程发送/回复
*/

#include "precompiled.h"
#include "Message.h"
#include "../Util/SpinLockQueue.h"
#include <chrono>

namespace bllsll {

class ActorSystem;

// [P2] A.9 Actor 运行指标
struct ActorMetrics {
    std::atomic<uint64_t> totalMsgProcessed{0};    // 累计处理消息数
    std::atomic<uint64_t> totalProcessTimeUs{0};    // 累计处理耗时（微秒）
    std::atomic<uint64_t> maxProcessTimeUs{0};      // 单条消息最大处理耗时（微秒）
    std::atomic<uint64_t> maxMailboxSize{0};         // 历史最大邮箱大小
    std::atomic<uint64_t> totalPriorityMsgProcessed{0}; // 累计处理优先级消息数

    void Reset() {
        totalMsgProcessed.store(0);
        totalProcessTimeUs.store(0);
        maxProcessTimeUs.store(0);
        maxMailboxSize.store(0);
        totalPriorityMsgProcessed.store(0);
    }

    // 平均处理耗时（微秒）
    uint64_t AvgProcessTimeUs() const {
        uint64_t total = totalMsgProcessed.load();
        if (total == 0) return 0;
        return totalProcessTimeUs.load() / total;
    }
};

class Actor
{
public:
    Actor() = default;
    virtual ~Actor() = default;

    uint32_t GetActorId() const { return actorId_; }
    void SetActorId(uint32_t id) { actorId_ = id; }
    void SetSystem(ActorSystem* sys) { system_ = sys; }
    ActorSystem* GetSystem() { return system_; }

    // 往邮箱投递消息（[P1] 内含高水位告警，[P3] 根据 priority 字段路由到对应队列）
    void PushMessage(ActorMessage&& msg);
    // 从邮箱取一条消息处理，返回是否处理了消息（[P3] 优先取优先级队列，[P2] 内含耗时统计）
    bool ProcessOne();
    // 邮箱是否为空（两个队列都为空）
    bool IsMailboxEmpty() const;
    // [P1] 获取当前邮箱大小（两个队列之和）
    size_t GetMailboxSize() const;

    // 调度标记（用于ActorSystem的就绪队列去重）
    std::atomic<bool> scheduled_{false};

    // 子类实现：处理消息
    virtual void OnMessage(ActorMessage& msg) = 0;

    // [P1] 设置邮箱高水位告警阈值（0=禁用，默认禁用）
    void SetMailboxHighWaterMark(size_t hwm) { mailboxHighWaterMark_ = hwm; }

    // [P2] A.9 获取指标
    const ActorMetrics& GetMetrics() const { return metrics_; }
    ActorMetrics& GetMetrics() { return metrics_; }

protected:
    // 发消息给其他Actor
    void SendToActor(uint32_t targetId, ActorMessage&& msg);
    // [P3] A.10 发送高优先级消息给其他Actor
    void SendPriorityToActor(uint32_t targetId, ActorMessage&& msg);
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

    // ===== [P3] A.11 跨进程集群辅助方法 =====
    // 跨进程发送消息（按远程节点ID + Actor名字寻址）
    bool SendToRemote(const std::string& targetNodeId, const std::string& targetActorName,
                      ActorMessage&& msg);
    // 回复跨进程消息（根据 msg 中的 sourceNodeId + sourceActorName 自动路由回去）
    bool RespondRemote(const ActorMessage& request, const std::string& responseData);

    uint32_t actorId_ = 0;
    ActorSystem* system_ = nullptr;
    size_t mailboxHighWaterMark_ = 0;  // [P1] 高水位告警阈值（0=禁用）

    // [P2] A.9 运行指标
    ActorMetrics metrics_;

private:
    // [P3] A.10 双队列邮箱
    bllsll::SpinLockQueue<ActorMessage> priorityMailbox_;   // 高优先级队列
    bllsll::SpinLockQueue<ActorMessage> normalMailbox_;     // 普通队列

    // 向后兼容：旧代码中 mailbox_ 作为 normalMailbox_ 的别名
    // 注意：mailbox_ 已被替换为 normalMailbox_ + priorityMailbox_
};

} // namespace bllsll
