#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: 协程Actor基类 — Skynet 风格的服务协程

核心机制：
  1. 每条消息到达时，OnMessage() 检查 isResponse 标志：
     - 如果是 Response（isResponse=true）→ 恢复等待该 sessionId 的协程
     - 如果是新消息 → 调用 OnCoroutineMessage() 创建新协程
  
  2. OnCoroutineMessage() 是用户重写的协程函数（返回 ActorTask）：
     - 可以使用 co_await Call(targetId, msg) 发送消息并等待响应
     - 可以使用 co_await Sleep(ms) 休眠
     - 挂起时 worker 线程释放，可处理其他 Actor
  
  3. 多个协程可以在同一个 Actor 上并发挂起（类似 Skynet）：
     - 消息 A 到达 → 创建协程 A → co_await Call → 挂起
     - 消息 B 到达 → 创建协程 B → co_await Call → 挂起
     - 响应 A 到达 → 恢复协程 A → 继续执行
     - 响应 B 到达 → 恢复协程 B → 继续执行
     但同一时刻只有一个协程在运行（邮箱串行处理保证）

改进记录：
  [P1] A.7 优雅关停 — DestroyAllCoroutines()：
       在 ActorSystem::Stop() 排空邮箱后，销毁所有悬挂的协程帧，防止内存泄漏。
  [P0] A.3 Sleep() 改用 TimerManager：
       不再启动 detached thread，而是通过 ActorSystem::SetTimeout() 注册定时器。

使用示例：
  class MyService : public CoroutineActor {
      ActorTask OnCoroutineMessage(ActorMessage msg) override {
          // 同步风格的异步代码！
          auto resp = co_await Call(dbActorId,
              ActorMessage{MsgType::UserMessage, 0, -1, "get:player_level"});
          std::cout << "level = " << resp.data << std::endl;
          
          co_await Sleep(100);  // 休眠 100ms
          
          // 可以 Respond 给调用者（如果有人 Call 了我们）
          Respond(msg, ActorMessage{MsgType::UserMessage, 0, -1, "done"});
      }
  };
*/

#include "Actor.h"
#include "Coroutine.h"
#include <coroutine>
#include <unordered_map>

namespace bllsll {

class CoroutineActor : public Actor
{
    // Awaiter 需要访问内部方法
    friend struct CallAwaiter;
    friend struct SleepAwaiter;

public:
    CoroutineActor() = default;
    virtual ~CoroutineActor();

    // ===== Actor::OnMessage 重写（final，子类不可再重写）=====
    // 分发逻辑：Response → 恢复协程，新消息 → 创建协程
    void OnMessage(ActorMessage& msg) override final;

    // ===== 用户重写：协程消息处理器 =====
    // 参数按值传递（协程可能挂起，原引用会失效）
    virtual ActorTask OnCoroutineMessage(ActorMessage msg) = 0;

    // ===== Skynet 风格 API =====

    // 类似 skynet.call()：发送消息并等待响应
    // 用法: auto resp = co_await Call(targetId, msg);
    CallAwaiter Call(uint32_t targetId, ActorMessage&& msg);

    // 类似 skynet.ret()：响应一个 Call 请求
    // 用法: Respond(originalMsg, responseMsg);
    void Respond(const ActorMessage& request, ActorMessage&& response);

    // 类似 skynet.sleep()：协程休眠
    // 用法: co_await Sleep(100);  // 休眠 100ms
    SleepAwaiter Sleep(int ms);

    // [P1] 优雅关停：销毁所有未完成的协程帧，清理等待映射
    // 由 ActorSystem::Stop() 在排空邮箱后调用，防止协程帧泄漏
    void DestroyAllCoroutines();

private:
    // ===== 内部协程管理 =====

    // 分配唯一 sessionId
    uint32_t AllocSession() { return nextSessionId_++; }

    // 存储挂起的协程句柄
    void StoreWaiting(uint32_t session, std::coroutine_handle<> h);

    // 恢复挂起的协程（由 OnMessage 在收到 Response 时调用）
    bool ResumeWaiting(uint32_t session, ActorMessage&& response);

    uint32_t nextSessionId_ = 1;
    // sessionId → 挂起的协程句柄
    std::unordered_map<uint32_t, std::coroutine_handle<>> waitMap_;
    // sessionId → 响应消息（供 await_resume 取出）
    std::unordered_map<uint32_t, ActorMessage> responseMap_;
};

} // namespace bllsll
