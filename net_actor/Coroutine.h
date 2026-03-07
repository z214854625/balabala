#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: C++20 协程支持 — Skynet 风格的服务协程调用
        定义 ActorTask（协程返回类型）、CallAwaiter（阻塞式RPC）、SleepAwaiter（协程休眠）

核心思想（对标 Skynet）：
  Skynet:  skynet.call(addr, ...)  -- 发消息并挂起当前协程，等待响应
           skynet.ret(...)          -- 响应调用者
           skynet.sleep(n)          -- 协程休眠
  
  本框架: co_await Call(actorId, msg)  -- 发消息并挂起当前协程，等待响应
          RespondToCall(req, resp)    -- 响应调用者
          co_await Sleep(ms)          -- 协程休眠
*/

#include <coroutine>
#include "Message.h"

namespace bllsll {

// 前置声明
class CoroutineActor;

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
//  CallAwaiter: co_await Call(targetId, msg)
//
//  类似 Skynet 的 skynet.call()：
//  1. 分配 sessionId
//  2. 将当前协程句柄存入 waitMap_
//  3. 发送消息给目标 Actor（带 sessionId）
//  4. 挂起当前协程
//  5. 目标 Actor 处理后调用 RespondToCall() 发回响应
//  6. 响应到达邮箱 → OnMessage() 检测 isResponse → 恢复协程
//  7. await_resume() 返回响应消息
// ============================================================
struct CallAwaiter
{
    CoroutineActor* actor;
    uint32_t targetId;
    ActorMessage msg;
    uint32_t sessionId = 0;

    // 永远不会立即就绪（必须发送消息并等待）
    bool await_ready() const noexcept { return false; }
    // 挂起时：注册等待、发送消息
    void await_suspend(std::coroutine_handle<> h);
    // 恢复时：取出响应消息
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
    CoroutineActor* actor;
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
