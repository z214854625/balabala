/**
 * @file CoroutineActor.cc
 * @brief CoroutineActor 实现 + CallAwaiter / ClusterCallAwaiter / SleepAwaiter 实现
 *
 * 协程生命周期：
 *   1. 新消息到达 → OnMessage() → OnCoroutineMessage(move(msg))
 *      → 协程帧在堆上分配（编译器自动），立即执行（initial_suspend = suspend_never）
 *   2. 遇到 co_await Call(...) 或 co_await ClusterCall(...) → await_suspend:
 *      → 分配 sessionId，存储 coroutine_handle 到 waitMap_
 *      → 发送消息给目标 Actor（本地或远程）
 *      → 返回（协程挂起，OnMessage 返回，ProcessOne 返回，worker 线程释放）
 *   3. 目标 Actor 处理完毕，调用 RespondToCall() 或 RespondRemote() 发回响应
 *   4. 响应消息到达邮箱 → ProcessOne → OnMessage
 *      → 检测 isResponse=true → ResumeWaiting:
 *      → 从 waitMap_ 取出 coroutine_handle
 *      → 存储响应到 responseMap_
 *      → h.resume()（协程从 co_await 处继续执行）
 *   5. 协程执行到 co_return 或函数末尾
 *      → final_suspend = suspend_never → 协程帧自动销毁
 *
 * 改进记录：
 *   [P0] SleepAwaiter 改用 TimerManager（ActorSystem::SetTimeout），
 *        不再启动 detached thread
 *   [P1] 添加 DestroyAllCoroutines() 用于优雅关停时清理悬挂协程
 *   [P3] 添加 ClusterCallAwaiter 支持跨进程协程 RPC（co_await ClusterCall）
 */

#include "CoroutineActor.h"
#include "ActorSystem.h"

using namespace bllsll;

// ================================================================
//  CoroutineActor 析构
// ================================================================

CoroutineActor::~CoroutineActor()
{
    // 析构时清理所有未完成的协程帧
    DestroyAllCoroutines();
}

// ================================================================
//  CoroutineActor 实现
// ================================================================

void CoroutineActor::OnMessage(ActorMessage& msg)
{
    if (msg.isResponse && msg.sessionId > 0) {
        // 这是一个 Response 消息 — 恢复等待的协程
        if (!ResumeWaiting(msg.sessionId, std::move(msg))) {
            std::cout << "[CoroutineActor:" << actorId_
                      << "] no coroutine waiting for session=" << msg.sessionId << std::endl;
        }
    } else {
        // 新消息 — 创建新协程处理
        // 参数按值传递，因为协程可能挂起，原引用会在 ProcessOne() 返回后失效
        OnCoroutineMessage(std::move(msg));
    }
}

CallAwaiter CoroutineActor::Call(uint32_t targetId, ActorMessage&& msg)
{
    return CallAwaiter{this, targetId, std::move(msg)};
}

ClusterCallAwaiter CoroutineActor::ClusterCall(const std::string& targetNodeId,
                                                const std::string& targetActorName,
                                                ActorMessage&& msg)
{
    return ClusterCallAwaiter{this, targetNodeId, targetActorName, std::move(msg)};
}

void CoroutineActor::Respond(const ActorMessage& request, ActorMessage&& response)
{
    // 委托给基类的 RespondToCall
    RespondToCall(request, std::move(response));
}

SleepAwaiter CoroutineActor::Sleep(int ms)
{
    return SleepAwaiter{this, ms};
}

void CoroutineActor::StoreWaiting(uint32_t session, std::coroutine_handle<> h)
{
    waitMap_[session] = h;
}

bool CoroutineActor::ResumeWaiting(uint32_t session, ActorMessage&& response)
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

// ================================================================
//  [P1] DestroyAllCoroutines — 优雅关停时清理悬挂协程
//  在 ActorSystem::Stop() 排空邮箱后、销毁 Actor 之前调用。
//  对所有仍挂起的协程帧执行 destroy()，防止内存泄漏。
// ================================================================

void CoroutineActor::DestroyAllCoroutines()
{
    for (auto& [session, h] : waitMap_) {
        if (h && !h.done()) {
            std::cout << "[CoroutineActor:" << actorId_
                      << "] destroying pending coroutine session=" << session << std::endl;
            h.destroy();
        }
    }
    waitMap_.clear();
    responseMap_.clear();
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
    // SendToActor 是 Actor 的 protected 方法，CallAwaiter 是 CoroutineActor 的 friend，
    // 根据 C++ 标准 [class.friend]/2，friend 拥有与成员相同的访问权限
    actor->SendToActor(targetId, std::move(msg));

    // 4. 返回后协程挂起，worker 线程释放
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
    return result;
}

// ================================================================
//  ClusterCallAwaiter 实现 — 跨进程协程 RPC
//
//  与 CallAwaiter 的唯一区别：
//    CallAwaiter:        actor->SendToActor(targetId, msg)      本地路由
//    ClusterCallAwaiter: actor->SendToRemote(nodeId, name, msg) TCP跨进程
//
//  回程路径（自动工作，无需额外代码）：
//    远端 RespondRemote(msg, data)
//    → TCP 回传 ClusterPacket（isResponse=true, sessionId）
//    → ClusterGatewayActor::handleDecodedPacket
//    → SendByName(targetActorName) 路由到本地调用方
//    → CoroutineActor::OnMessage 检测 isResponse → ResumeWaiting
//    → 协程恢复 → await_resume() 返回响应
// ================================================================

void ClusterCallAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 1. 分配 sessionId
    sessionId = actor->AllocSession();

    // 2. 存储协程句柄（等待恢复）
    actor->StoreWaiting(sessionId, h);

    // 3. 设置消息的 sessionId，通过 SendToRemote 发送到远端节点
    msg.sessionId = sessionId;
    // SendToRemote 是 Actor 的 protected 方法，ClusterCallAwaiter 是 CoroutineActor 的 friend
    actor->SendToRemote(targetNodeId, targetActorName, std::move(msg));

    // 4. 返回后协程挂起，worker 线程释放
    //    远端 Actor 处理后调用 RespondRemote()，响应通过 TCP 返回
    //    ClusterGatewayActor 解码后 SendByName 路由到本 Actor 的邮箱
    //    OnMessage 检测 isResponse → ResumeWaiting → 协程恢复
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
