#include "ActorSystem.h"
#include "EventLoop.h"
#include "ClusterProxy.h"

using namespace bllsll;
using namespace std;

ActorSystem::ActorSystem()
{
}

ActorSystem::~ActorSystem()
{
    Stop();
}

void ActorSystem::Start(int workerCount, EventLoop* loop)
{
    loop_ = loop;
    running_ = true;
    // [P0] 启动定时器管理器
    timerManager_.Start(this);
    for (int i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
    std::cout << "ActorSystem started with " << workerCount << " workers." << std::endl;
}

void ActorSystem::Stop()
{
    if (!running_.exchange(false)) {
        return;  // 已经停止，避免重复调用
    }

    // [P1] 优雅关停 Phase 1: 停止定时器（不再产生新消息）
    timerManager_.Stop();

    // Phase 2: 停止 worker 线程
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();

    // Phase 3: 排空剩余邮箱（在主线程处理，最多处理 maxDrainRounds 轮避免无限循环）
    const int maxDrainRounds = 3;
    for (int round = 0; round < maxDrainRounds; ++round) {
        bool anyProcessed = false;
        bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
        for (auto& [id, actor] : actors_) {
            int drained = 0;
            while (!actor->IsMailboxEmpty() && drained < 64) {
                try {
                    actor->ProcessOne();
                } catch (const std::exception& e) {
                    std::cerr << "[ActorSystem::Stop] drain exception in actor " << id
                              << ": " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[ActorSystem::Stop] drain unknown exception in actor " << id << std::endl;
                }
                ++drained;
                anyProcessed = true;
            }
        }
        if (!anyProcessed) break;
    }

    // Phase 4: 记录未处理消息数量并清理
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
        for (auto& [id, actor] : actors_) {
            if (!actor->IsMailboxEmpty()) {
                std::cerr << "[ActorSystem::Stop] actor " << id
                          << " still has unprocessed messages (mailbox not empty)" << std::endl;
            }
        }
    }

    // Phase 5: 清理名字映射和 Link 注册表
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
        nameToActor_.clear();
        actorToName_.clear();
    }
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(linkLock_);
        linkMap_.clear();
    }

    std::cout << "ActorSystem stopped." << std::endl;
}

uint32_t ActorSystem::RegisterActor(std::unique_ptr<Actor> actor)
{
    uint32_t id = nextActorId_.fetch_add(1);
    actor->SetActorId(id);
    actor->SetSystem(this);
    bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
    actors_[id] = std::move(actor);
    std::cout << "RegisterActor id=" << id << std::endl;
    return id;
}

void ActorSystem::UnregisterActor(uint32_t actorId)
{
    // [P2] Link 通知：在删除前，通知所有监控该 Actor 的 watcher
    notifyActorDown(actorId, "unregistered");

    // 清理名字映射
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
        auto nameIt = actorToName_.find(actorId);
        if (nameIt != actorToName_.end()) {
            nameToActor_.erase(nameIt->second);
            actorToName_.erase(nameIt);
        }
    }

    // 清理 Link 注册表中该 Actor 作为 watcher 的条目
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(linkLock_);
        // 移除该 Actor 作为 target 的记录（已在 notifyActorDown 中取出）
        linkMap_.erase(actorId);
        // 移除该 Actor 作为 watcher 出现在其他 target 的监控列表中
        for (auto& [targetId, watchers] : linkMap_) {
            watchers.erase(actorId);
        }
    }

    bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
    actors_.erase(actorId);
    std::cout << "UnregisterActor id=" << actorId << std::endl;
}

void ActorSystem::Send(uint32_t actorId, ActorMessage&& msg)
{
    Actor* actor = nullptr;
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
        auto it = actors_.find(actorId);
        if (it == actors_.end()) {
            std::cout << "ActorSystem::Send actor not found! id=" << actorId << std::endl;
            return;
        }
        actor = it->second.get();
    }

    actor->PushMessage(std::move(msg));
    // 如果actor未被调度，加入就绪队列
    bool expected = false;
    if (actor->scheduled_.compare_exchange_strong(expected, true)) {
        readyQueue_.push(actorId);
        cv_.notify_one();
    }
}

// [P3] A.10 发送高优先级消息
void ActorSystem::SendPriority(uint32_t actorId, ActorMessage&& msg)
{
    msg.priority = true;
    Send(actorId, std::move(msg));
}

Actor* ActorSystem::GetActor(uint32_t actorId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
    auto it = actors_.find(actorId);
    if (it == actors_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void ActorSystem::BindFdToActor(int fd, uint32_t actorId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(fdMapLock_);
    fdToActor_[fd] = actorId;
    std::cout << "BindFdToActor fd=" << fd << ", actorId=" << actorId << std::endl;
}

void ActorSystem::UnbindFd(int fd)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(fdMapLock_);
    fdToActor_.erase(fd);
    std::cout << "UnbindFd fd=" << fd << std::endl;
}

uint32_t ActorSystem::GetActorIdByFd(int fd)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(fdMapLock_);
    auto it = fdToActor_.find(fd);
    if (it == fdToActor_.end()) {
        return 0;
    }
    return it->second;
}

void ActorSystem::SendByFd(int fd, ActorMessage&& msg)
{
    uint32_t actorId = GetActorIdByFd(fd);
    if (actorId > 0) {
        Send(actorId, std::move(msg));
    } else {
        std::cout << "ActorSystem::SendByFd no actor bound to fd=" << fd << std::endl;
    }
}

// ================================================================
//  [P0] 定时器管理（委托给 TimerManager）
// ================================================================

uint64_t ActorSystem::SetTimeout(uint32_t actorId, int delayMs, ActorMessage&& msg)
{
    return timerManager_.SetTimeout(actorId, delayMs, std::move(msg));
}

uint64_t ActorSystem::SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg)
{
    return timerManager_.SetInterval(actorId, intervalMs, std::move(msg));
}

void ActorSystem::CancelTimer(uint64_t timerId)
{
    timerManager_.CancelTimer(timerId);
}

// ================================================================
//  [P1] Actor 命名/发现
// ================================================================

void ActorSystem::RegisterName(const std::string& name, uint32_t actorId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
    // 如果名字已被占用，先清理旧映射
    auto it = nameToActor_.find(name);
    if (it != nameToActor_.end()) {
        actorToName_.erase(it->second);
    }
    nameToActor_[name] = actorId;
    actorToName_[actorId] = name;
    std::cout << "RegisterName \"" << name << "\" -> actorId=" << actorId << std::endl;
}

void ActorSystem::UnregisterName(const std::string& name)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
    auto it = nameToActor_.find(name);
    if (it != nameToActor_.end()) {
        actorToName_.erase(it->second);
        nameToActor_.erase(it);
    }
}

uint32_t ActorSystem::FindActor(const std::string& name)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
    auto it = nameToActor_.find(name);
    if (it == nameToActor_.end()) {
        return 0;
    }
    return it->second;
}

bool ActorSystem::SendByName(const std::string& name, ActorMessage&& msg)
{
    uint32_t actorId = FindActor(name);
    if (actorId > 0) {
        Send(actorId, std::move(msg));
        return true;
    }
    std::cout << "ActorSystem::SendByName actor not found! name=\"" << name << "\"" << std::endl;
    return false;
}

// ================================================================
//  [P2] Actor 监控/Link
// ================================================================

size_t ActorSystem::GetActorCount() const
{
    bllsll::LockGuard<bllsll::SpinLock> lock(const_cast<bllsll::SpinLock&>(actorsLock_));
    return actors_.size();
}

void ActorSystem::LinkActor(uint32_t watcherId, uint32_t targetId)
{
    if (watcherId == targetId) {
        std::cout << "[ActorSystem::LinkActor] cannot link actor to itself! id=" << watcherId << std::endl;
        return;
    }
    bllsll::LockGuard<bllsll::SpinLock> lock(linkLock_);
    linkMap_[targetId].insert(watcherId);
    std::cout << "[ActorSystem::LinkActor] watcher=" << watcherId << " -> target=" << targetId << std::endl;
}

void ActorSystem::UnlinkActor(uint32_t watcherId, uint32_t targetId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(linkLock_);
    auto it = linkMap_.find(targetId);
    if (it != linkMap_.end()) {
        it->second.erase(watcherId);
        if (it->second.empty()) {
            linkMap_.erase(it);
        }
    }
    std::cout << "[ActorSystem::UnlinkActor] watcher=" << watcherId << " -> target=" << targetId << std::endl;
}

void ActorSystem::notifyActorDown(uint32_t targetId, const std::string& reason)
{
    // 取出所有 watcher（在 linkLock_ 下操作），然后在锁外发送消息（避免死锁）
    std::set<uint32_t> watchers;
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(linkLock_);
        auto it = linkMap_.find(targetId);
        if (it != linkMap_.end()) {
            watchers = std::move(it->second);
            linkMap_.erase(it);
        }
    }

    // 在锁外发送 ActorDown 消息
    for (uint32_t watcherId : watchers) {
        std::string data = std::to_string(targetId) + ":" + reason;
        Send(watcherId, ActorMessage{MsgType::ActorDown, targetId, -1, std::move(data)});
        std::cout << "[ActorSystem] ActorDown notification: target=" << targetId
                  << " -> watcher=" << watcherId << ", reason=" << reason << std::endl;
    }
}

uint64_t ActorSystem::StartMonitor(uint32_t reportActorId, int intervalMs)
{
    // 使用 SetInterval 周期性给 reportActorId 发送 __monitor_tick__ 消息
    // 监控Actor收到此消息后调用 CollectActorStats() 遍历所有Actor状态
    return SetInterval(reportActorId, intervalMs, ActorMessage{
        MsgType::UserMessage, 0, -1, "__monitor_tick__"
    });
}

std::vector<ActorStat> ActorSystem::CollectActorStats()
{
    std::vector<ActorStat> stats;

    // Phase 1: 在 actorsLock_ 下收集 Actor 基础信息 + [P2] A.9 指标数据
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
        stats.reserve(actors_.size());
        for (auto& [id, actor] : actors_) {
            ActorStat stat;
            stat.actorId = id;
            stat.mailboxSize = actor->GetMailboxSize();
            stat.scheduled = actor->scheduled_.load();

            // [P2] A.9 从 ActorMetrics 获取指标数据
            const auto& metrics = actor->GetMetrics();
            stat.totalMsgProcessed = metrics.totalMsgProcessed.load();
            stat.totalProcessTimeUs = metrics.totalProcessTimeUs.load();
            stat.maxProcessTimeUs = metrics.maxProcessTimeUs.load();
            stat.avgProcessTimeUs = metrics.AvgProcessTimeUs();
            stat.maxMailboxSize = metrics.maxMailboxSize.load();
            stat.totalPriorityMsgProcessed = metrics.totalPriorityMsgProcessed.load();

            stats.push_back(std::move(stat));
        }
    }

    // Phase 2: 在 nameLock_ 下填充名字（避免嵌套锁）
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
        for (auto& stat : stats) {
            auto it = actorToName_.find(stat.actorId);
            if (it != actorToName_.end()) {
                stat.name = it->second;
            }
        }
    }

    return stats;
}

// ================================================================
//  [P1] 按 actorId 反查名字
// ================================================================

std::string ActorSystem::GetActorName(uint32_t actorId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(nameLock_);
    auto it = actorToName_.find(actorId);
    if (it != actorToName_.end()) {
        return it->second;
    }
    return "";
}

// ================================================================
//  [P3] A.11 跨进程集群
// ================================================================

void ActorSystem::RegisterTransport(IClusterTransport* transport)
{
    if (!transport) return;
    bllsll::LockGuard<bllsll::SpinLock> lock(transportLock_);
    // 避免重复注册
    for (auto* t : transports_) {
        if (t == transport) return;
    }
    transports_.push_back(transport);
    std::cout << "[ActorSystem] RegisterTransport nodeId=" << transport->GetLocalNodeId() << std::endl;
}

bool ActorSystem::SendToRemote(const std::string& targetNodeId, const std::string& targetActorName,
                                ActorMessage&& msg, const std::string& senderName)
{
    // 确定发送方名字（优先使用显式传入的 senderName，否则自动查找）
    std::string resolvedSenderName = senderName;
    if (resolvedSenderName.empty() && msg.sourceId > 0) {
        resolvedSenderName = GetActorName(msg.sourceId);
    }

    // 查找可以到达 targetNodeId 的 transport
    bllsll::LockGuard<bllsll::SpinLock> lock(transportLock_);
    for (auto* transport : transports_) {
        if (!transport) continue;
        // 构建 ClusterPacket
        ClusterPacket pkt;
        pkt.sourceNodeId = transport->GetLocalNodeId();
        pkt.targetNodeId = targetNodeId;
        pkt.sourceActorName = resolvedSenderName;
        pkt.targetActorName = targetActorName;
        pkt.sessionId = msg.sessionId;
        pkt.isResponse = msg.isResponse;
        pkt.data = std::move(msg.data);

        if (transport->SendPacket(pkt)) {
            return true;
        }
    }
    std::cerr << "[ActorSystem::SendToRemote] no transport for nodeId=" << targetNodeId << std::endl;
    return false;
}

// ================================================================
//  Worker 线程主循环
//  [P0] 异常保护：try-catch 包裹 ProcessOne，防止单个 Actor 的异常
//       导致 worker 线程退出
// ================================================================

void ActorSystem::workerLoop()
{
    while (running_) {
        auto optId = readyQueue_.pop();
        if (!optId) {
            // 等待通知
            std::unique_lock<std::mutex> lock(cvMutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }
        uint32_t actorId = *optId;
        Actor* actor = nullptr;
        {
            bllsll::LockGuard<bllsll::SpinLock> lock(actorsLock_);
            auto it = actors_.find(actorId);
            if (it == actors_.end()) {
                continue;
            }
            actor = it->second.get();
        }

        // [P0] 异常保护：批量处理消息时捕获异常
        int processed = 0;
        try {
            while (actor->ProcessOne() && ++processed < 64) {}
        } catch (const std::exception& e) {
            std::cerr << "[ActorSystem] Actor " << actorId
                      << " OnMessage exception: " << e.what() << std::endl;
            // Actor 继续存活，下次调度时继续处理后续消息
            // 出异常的那条消息被丢弃（已从邮箱 pop 出来）
        } catch (...) {
            std::cerr << "[ActorSystem] Actor " << actorId
                      << " OnMessage unknown exception!" << std::endl;
        }

        // 检查是否还有消息
        if (!actor->IsMailboxEmpty()) {
            // 还有消息，重新加入就绪队列
            readyQueue_.push(actorId);
            cv_.notify_one();
        } else {
            // 邮箱为空，取消调度标记
            actor->scheduled_.store(false);
            // 双重检查：防止在取消标记和检查之间有新消息到达
            if (!actor->IsMailboxEmpty()) {
                bool exp = false;
                if (actor->scheduled_.compare_exchange_strong(exp, true)) {
                    readyQueue_.push(actorId);
                    cv_.notify_one();
                }
            }
        }
    }
}
