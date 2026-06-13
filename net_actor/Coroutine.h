#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: C++20 协程支持 — Skynet 风格的服务协程调用
        定义 ActorTask（协程返回类型）、CallAwaiter（本地阻塞式RPC）、
        ClusterCallAwaiter（跨进程阻塞式RPC）、SleepAwaiter（协程休眠）

核心思想（对标 Skynet）：
  Skynet:  skynet.call(addr, ...)         -- 发消息并挂起当前协程，等待响应
           skynet.ret(...)                -- 响应调用者
           skynet.sleep(n)                -- 协程休眠
  
  本框架: co_await Call(actorId, msg)     -- 本地RPC：发消息并挂起，等待响应
          co_await ClusterCall(nodeId, actorName, msg)  -- 跨进程RPC：发消息到远端并挂起
          RespondToCall(req, resp)        -- 响应调用者（本地）
          RespondRemote(req, data)        -- 响应调用者（跨进程）
          co_await Sleep(ms)              -- 协程休眠

[合并] 原 CoroutineActor 已合并入 Actor，所有 Actor 天然支持协程
*/

#include <coroutine>
#include "Message.h"

namespace bllsll {

// 前置声明（合并后统一使用 Actor）
class Actor;

// ============================================================
//  ActorTask: 协程返回类型（fire-and-forget）
//
//  每条消息到达时创建一个协程，协程可以挂起（co_await Call/Sleep），
//  挂起后 worker 线程释放，可处理其他 Actor。
//  当响应到达时，协程恢复继续执行。
//  协程完成后自动销毁（final_suspend = suspend_never）。
// ============================================================
struct ActorTask
{
    struct promise_type
    {
        ActorTask get_return_object() { return {}; }
        // 立即执行，不在初始挂起点暂停
        std::suspend_never initial_suspend() noexcept { return {}; }
        // 完成后自动销毁协程帧
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
    // fire-and-forget，不持有 coroutine_handle
    // 协程帧的生命周期由挂起点和 final_suspend 管理
};

// ============================================================
//  CallAwaiter: co_await Call(targetId, msg, timeoutMs)
//
//  类似 Skynet 的 skynet.call()：
//  1. 分配 sessionId
//  2. 将当前协程句柄存入 waitMap_
//  3. 发送消息给目标 Actor（带 sessionId）
//  4. 若 timeoutMs > 0，注册一个超时定时器（到期后发回 isResponse + error=Timeout 的假响应）
//  5. 挂起当前协程
//  6. 目标 Actor 处理后调用 RespondToCall() 发回响应，或定时器先到 → 任一恢复协程
//  7. await_resume() 返回响应消息（业务侧通过 resp.error 判断结果状态）
//
//  超时语义：
//    - timeoutMs > 0  → 启用超时（默认 10s）
//    - timeoutMs <= 0 → 永久等待（escape hatch）
//    - 若真响应先到，await_resume 会取消定时器，避免之后的噪音消息
//
//  业务判错：
//    if (resp.error != CallError::Ok) {
//        // 处理失败：resp.error == CallError::Timeout / TargetMissing / ...
//    }
// ============================================================
struct CallAwaiter
{
    Actor* actor;
    uint32_t targetId;
    ActorMessage msg;
    int timeoutMs = 10000;
    uint32_t sessionId = 0;
    uint64_t timerId = 0;

    // 永远不会立即就绪（必须发送消息并等待）
    bool await_ready() const noexcept { return false; }
    // 挂起时：注册等待、发送消息、注册超时定时器
    void await_suspend(std::coroutine_handle<> h);
    // 恢复时：取出响应消息，必要时取消未触发的超时定时器
    ActorMessage await_resume();
};

// ============================================================
//  ClusterCallAwaiter: co_await ClusterCall(nodeId, actorName, msg)
//
//  跨进程版本的 CallAwaiter，类似 Skynet 的 cluster.call()：
//  1. 分配 sessionId
//  2. 将当前协程句柄存入 waitMap_
//  3. 调用 Actor::SendToRemote() 通过 TCP 发送到远端节点
//  4. 挂起当前协程
//  5. 远端 Actor 处理后调用 RespondRemote() 发回响应
//  6. 响应通过 TCP 返回 → ClusterGatewayActor 解码
//     → SendByName 路由到本地调用方 Actor 的邮箱
//  7. OnMessage() 检测 isResponse=true → ResumeWaiting → 协程恢复
//  8. await_resume() 返回响应消息
//
//  注意：调用方 Actor 必须已通过 RegisterName() 注册名字，
//        否则远端无法通过 sourceActorName 将响应路由回来！
// ============================================================
struct ClusterCallAwaiter
{
    Actor* actor;
    std::string targetNodeId;
    std::string targetActorName;
    ActorMessage msg;
    int timeoutMs = 10000;
    uint32_t sessionId = 0;
    uint64_t timerId = 0;

    // 永远不会立即就绪（必须发送到远端并等待）
    bool await_ready() const noexcept { return false; }
    // 挂起时：注册等待、通过 SendToRemote 发送跨进程消息、注册超时定时器
    void await_suspend(std::coroutine_handle<> h);
    // 恢复时：取出响应消息（与 CallAwaiter 相同的机制）
    ActorMessage await_resume();
};

// ============================================================
//  SleepAwaiter: co_await Sleep(milliseconds)
//
//  类似 Skynet 的 skynet.sleep(n)：
//  1. 分配 sessionId
//  2. 将当前协程句柄存入 waitMap_
//  3. 通过 ActorSystem::SetTimeout() 注册定时器（TimerManager 管理）
//  4. 挂起当前协程
//  5. 定时器到期后 TimerManager 发送 isResponse=true 的消息
//  6. 响应到达 → 恢复协程
//
//  [P0] 已改用 TimerManager 替代 detached thread
// ============================================================
struct SleepAwaiter
{
    Actor* actor;
    int milliseconds;
    uint32_t sessionId = 0;

    // 如果 ms <= 0，无需挂起
    bool await_ready() const noexcept { return milliseconds <= 0; }
    // 挂起时：注册等待、启动定时器
    void await_suspend(std::coroutine_handle<> h);
    // 恢复时：无返回值
    void await_resume() noexcept {}
};

} // namespace bllsll
