#include "ActorSystem.h"
#include "EventLoop.h"

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
    for (int i = 0; i < workerCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
    std::cout << "ActorSystem started with " << workerCount << " workers." << std::endl;
}

void ActorSystem::Stop()
{
    running_ = false;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
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

        // 批量处理消息（最多64条，避免饿死其他actor）
        int processed = 0;
        while (actor->ProcessOne() && ++processed < 64) {}

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
