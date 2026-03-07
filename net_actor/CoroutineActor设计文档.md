# C++20 协程 Actor 设计文档 — Skynet 风格的服务协程

## 目录

- [一、设计目标与 Skynet 对标](#一设计目标与-skynet-对标)
- [二、核心概念：为什么需要协程 Actor](#二核心概念为什么需要协程-actor)
- [三、整体架构](#三整体架构)
- [四、新增/修改文件清单](#四新增修改文件清单)
- [五、核心类型详解](#五核心类型详解)
- [六、协程生命周期：从消息到达到协程完成](#六协程生命周期从消息到达到协程完成)
- [七、Call 机制详解：co_await Call() 的完整流程](#七call-机制详解co_await-call-的完整流程)
- [八、Sleep 机制详解：co_await Sleep() 的完整流程](#八sleep-机制详解co_await-sleep-的完整流程)
- [九、并发协程：同一 Actor 上的多协程交错执行](#九并发协程同一-actor-上的多协程交错执行)
- [十、链式 Call：跨多层 Actor 的协程调用链](#十链式-call跨多层-actor-的协程调用链)
- [十一、何时用协程 Actor、何时用普通 Actor](#十一何时用协程-actor何时用普通-actor)
- [十二、线程安全分析](#十二线程安全分析)
- [十三、与原有 Actor 模型的兼容性](#十三与原有-actor-模型的兼容性)
- [十四、测试用例说明](#十四测试用例说明)
- [十五、生产环境优化建议](#十五生产环境优化建议)
- [附录 A：架构完善性分析 — 现有框架的不足与改进方向](#附录-a架构完善性分析--现有框架的不足与改进方向)

---

## 一、设计目标与 Skynet 对标

### 1.1 设计目标

在现有 Actor 模型基础上，利用 C++20 协程（coroutine）实现 **Skynet 风格的服务协程调用**：
- 服务（Actor）的消息处理函数运行在协程中
- 可以在消息处理过程中 **发送请求并等待响应**（阻塞当前协程，但不阻塞线程）
- 代码写起来像同步代码，实际运行是异步的

### 1.2 Skynet API 对照表

| Skynet Lua | 本框架 C++20 | 功能说明 |
|---|---|---|
| `skynet.call(addr, type, ...)` | `co_await Call(actorId, msg)` | 发送消息并挂起协程，等待目标 Actor 响应 |
| `skynet.ret(skynet.pack(...))` | `Respond(request, response)` | 响应一个 `Call` 请求 |
| `skynet.sleep(n)` | `co_await Sleep(ms)` | 协程休眠指定毫秒 |
| `skynet.send(addr, type, ...)` | `SendToActor(actorId, msg)` | fire-and-forget 发送消息（不等待响应） |
| `skynet.dispatch("lua", handler)` | `OnCoroutineMessage(msg)` override | 注册消息处理协程 |
| `skynet.register_protocol` | `MsgType` 枚举 | 消息类型定义 |

### 1.3 代码风格对比

**Skynet Lua 写法：**
```lua
function CMD.query_player(source, player_name)
    -- 阻塞式调用 DB 服务（挂起当前协程）
    local name  = skynet.call(db_service, "lua", "get", player_name .. "_name")
    local level = skynet.call(db_service, "lua", "get", player_name .. "_level")
    
    skynet.sleep(5)  -- 休眠 50ms
    
    -- 响应调用者
    skynet.ret(skynet.pack(name .. ":lv" .. level))
end
```

**本框架 C++20 写法：**
```cpp
ActorTask GameService::OnCoroutineMessage(ActorMessage msg) {
    if (msg.data == "query_player") {
        // 阻塞式调用 DB Actor（挂起当前协程）
        auto r1 = co_await Call(dbActorId,
            ActorMessage{MsgType::UserMessage, 0, -1, "get:player_name"});
        auto r2 = co_await Call(dbActorId,
            ActorMessage{MsgType::UserMessage, 0, -1, "get:player_level"});
        
        co_await Sleep(50);  // 休眠 50ms
        
        // 响应调用者
        Respond(msg, ActorMessage{MsgType::UserMessage, 0, -1,
            r1.data + ":lv" + r2.data});
    }
}
```

---

## 二、核心概念：为什么需要协程 Actor

### 2.1 原有 Actor 模型的局限

原有的 `Actor::OnMessage()` 是**纯同步函数**：

```
消息到达 → OnMessage() 执行 → 返回 → 处理下一条消息
```

如果一个 Actor 需要**查询另一个 Actor 并等待结果**，原有模型需要：

```
// 方式1：回调嵌套（回调地狱）
void OnMessage(ActorMessage& msg) {
    if (msg.data == "query") {
        SendToActor(dbId, {..."get:name"});  // 发出去
        // ❌ 没法在这里等结果！OnMessage 必须返回
    }
    if (msg.type == MsgType::UserMessage && msg.data.find("db_result:") == 0) {
        // ❌ 需要在另一个消息中处理结果，状态管理复杂
    }
}
```

```
// 方式2：状态机
void OnMessage(ActorMessage& msg) {
    switch (state_) {
    case IDLE:
        if (msg.data == "query") {
            SendToActor(dbId, {..."get:name"});
            state_ = WAIT_NAME;
        }
        break;
    case WAIT_NAME:
        name_ = msg.data;
        SendToActor(dbId, {..."get:level"});
        state_ = WAIT_LEVEL;
        break;
    case WAIT_LEVEL:
        level_ = msg.data;
        // 终于拿到所有数据了
        state_ = IDLE;
        break;
    }
}
// ❌ 状态爆炸！每增加一步查询，状态机复杂度翻倍
```

### 2.2 协程 Actor 的优势

使用 C++20 协程后，**多步异步操作写成顺序代码**：

```cpp
ActorTask GameService::OnCoroutineMessage(ActorMessage msg) {
    // 写起来像同步代码，但实际是异步执行
    auto name  = (co_await Call(dbId, {..."get:name"})).data;   // 挂起，等结果
    auto level = (co_await Call(dbId, {..."get:level"})).data;  // 挂起，等结果
    co_await Sleep(50);                                          // 挂起，等定时器
    // 拿到所有数据，继续处理
    std::cout << name << ":lv" << level << std::endl;
}
```

**关键优势：**

| 特性 | 回调/状态机 | 协程 Actor |
|---|---|---|
| 代码可读性 | 状态分散在多个 case 中 | 顺序代码，一目了然 |
| 状态管理 | 手动管理状态变量 | 编译器自动保存在协程帧中 |
| 错误处理 | 每个 case 都要处理 | 正常的 try-catch |
| 局部变量 | 需要提升为成员变量 | 正常使用局部变量 |
| 并发控制 | 复杂的状态机组合 | 多个协程自然交错 |

---

## 三、整体架构

### 3.1 类继承关系

```
                    IConnection (接口)
                   ╱            ╲
          ConnectionBase      Acceptor
         (读写逻辑基类)      (监听accept)
         ╱           ╲
    Connection     Connector
  (服务端连接)    (客户端连接)


              Actor (基类)
             ╱           ╲
    普通 Actor         CoroutineActor (新增)
   (同步 OnMessage)    (协程 OnCoroutineMessage)
   ╱        ╲               ╱           ╲
EchoServer  Database    GameService   MiddleService
  Actor      Actor        Actor         Actor
```

### 3.2 协程 Actor 在整体架构中的位置

```
╔══════════════════════════════════════════════════════════════════╗
║                   协程 Actor 架构（在原 Actor 模型基础上扩展）       ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║   ┌───────────────────────────────────────────────────────┐      ║
║   │                    ActorSystem                         │      ║
║   │  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  │      ║
║   │  │ DatabaseActor│  │ GameService │  │ MiddleService│  │      ║
║   │  │ (普通Actor)  │  │ (协程Actor) │  │ (协程Actor)  │  │      ║
║   │  │ OnMessage()  │  │ OnCoroutine │  │ OnCoroutine  │  │      ║
║   │  │  ├─get:key   │  │ Message()   │  │ Message()    │  │      ║
║   │  │  └─respond   │  │  ├─co_await │  │  ├─co_await  │  │      ║
║   │  │              │  │  │  Call()   │  │  │  Call()   │  │      ║
║   │  │              │  │  ├─co_await │  │  ├─Respond() │  │      ║
║   │  │              │  │  │  Sleep() │  │  └─...       │  │      ║
║   │  │              │  │  └─Respond()│  │              │  │      ║
║   │  └──────────────┘  └─────────────┘  └──────────────┘  │      ║
║   │                                                        │      ║
║   │  ┌────────────────────────────────────────────────┐   │      ║
║   │  │              Worker 线程池                       │   │      ║
║   │  │  从 readyQueue_ 取 actorId → ProcessOne()      │   │      ║
║   │  │  → OnMessage() → 可能创建/恢复协程              │   │      ║
║   │  └────────────────────────────────────────────────┘   │      ║
║   └───────────────────────────────────────────────────────┘      ║
║                                                                  ║
║   新增组件（红色部分）：                                            ║
║   • CoroutineActor — 协程 Actor 基类                              ║
║   • ActorTask — 协程返回类型                                       ║
║   • CallAwaiter — co_await Call() 的等待器                         ║
║   • SleepAwaiter — co_await Sleep() 的等待器                       ║
║   • Message.sessionId/isResponse — Call/Response 配对字段          ║
║   • Actor.RespondToCall() — 响应 Call 的辅助方法                    ║
╚══════════════════════════════════════════════════════════════════╝
```

---

## 四、新增/修改文件清单

### 4.1 修改的文件

| 文件 | 修改内容 | 原因 |
|---|---|---|
| `Message.h` | `ActorMessage` 新增 `sessionId` (uint32_t) 和 `isResponse` (bool) | Call/Response 配对需要会话标识。`sessionId` 唯一标识一次 `co_await Call()`，`isResponse` 区分请求和响应 |
| `Actor.h` | 新增 `RespondToCall()` protected 方法 | 使任何 Actor（包括非协程 Actor）都能响应 `co_await Call()`，只需设置 `sessionId + isResponse` 后发回 |
| `Actor.cc` | 实现 `RespondToCall()` | 自动填充 `response.sessionId = request.sessionId`，`response.isResponse = true`，发送给 `request.sourceId` |

### 4.2 新增的文件

| 文件 | 职责 |
|---|---|
| `Coroutine.h` | C++20 协程类型定义：`ActorTask`（返回类型）、`CallAwaiter`、`SleepAwaiter` |
| `CoroutineActor.h` | `CoroutineActor` 类声明：继承 `Actor`，重写 `OnMessage()`，提供协程 API |
| `CoroutineActor.cc` | `CoroutineActor` 实现 + `CallAwaiter` / `SleepAwaiter` 的 `await_suspend` / `await_resume` |
| `test_coroutine_actor.cc` | 5 个测试用例，覆盖 Call、Sleep、并发、链式调用、综合场景 |
| `Makefile.coroutine` | 构建文件（`-std=c++20 -fcoroutines`） |
| `run_test_coroutine.sh` | 一键构建 & 运行脚本 |

---

## 五、核心类型详解

### 5.1 ActorTask — 协程返回类型

```cpp
// Coroutine.h
struct ActorTask {
    struct promise_type {
        ActorTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }   // 立即执行
        std::suspend_never final_suspend() noexcept { return {}; }     // 完成后自动销毁
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};
```

**设计决策：**

| 决策 | 选择 | 理由 |
|---|---|---|
| `initial_suspend` | `suspend_never` | 协程创建后立即执行，类似 Skynet 的消息 dispatch |
| `final_suspend` | `suspend_never` | 协程完成后自动销毁协程帧，无需手动管理 |
| 是否存储 handle | 否 | fire-and-forget 模式，协程帧由挂起点和 final_suspend 管理 |

**协程帧生命周期：**

```
OnCoroutineMessage(msg)
  → 编译器在堆上分配协程帧（存储局部变量、挂起点）
  → initial_suspend = suspend_never → 立即开始执行
  → 遇到 co_await → 协程挂起，handle 存入 waitMap_
  → ...（其他消息处理）...
  → 响应到达 → h.resume() → 协程继续
  → 到达 co_return 或函数末尾
  → final_suspend = suspend_never → 协程帧自动释放
```

### 5.2 CallAwaiter — co_await Call() 的等待器

```cpp
// Coroutine.h
struct CallAwaiter {
    CoroutineActor* actor;     // 所属的协程 Actor
    uint32_t targetId;         // 目标 Actor ID
    ActorMessage msg;          // 要发送的消息
    uint32_t sessionId = 0;    // 分配的会话 ID

    bool await_ready() const noexcept { return false; }  // 永远需要挂起
    void await_suspend(std::coroutine_handle<> h);       // 注册等待 + 发送消息
    ActorMessage await_resume();                          // 取出响应消息
};
```

**三个 await 方法的职责：**

```
co_await Call(targetId, msg)
     │
     ▼
 await_ready() → false（永远需要挂起和等待）
     │
     ▼
 await_suspend(coroutine_handle h):
   1. sessionId = actor->AllocSession()        // 分配唯一会话 ID
   2. actor->StoreWaiting(sessionId, h)        // 存储协程句柄到 waitMap_
   3. msg.sessionId = sessionId                // 在消息中标记会话
   4. actor->SendToActor(targetId, move(msg))  // 发送给目标 Actor
   5. return（协程挂起，控制权回到 OnMessage 调用者）
     │
     │  ...等待响应...
     │
     ▼
 await_resume():（响应到达后，协程被 resume，从这里继续）
   1. 从 responseMap_[sessionId] 取出响应
   2. 清理 responseMap_ 条目
   3. return 响应 ActorMessage
```

### 5.3 SleepAwaiter — co_await Sleep() 的等待器

```cpp
// Coroutine.h
struct SleepAwaiter {
    CoroutineActor* actor;
    int milliseconds;
    uint32_t sessionId = 0;

    bool await_ready() const noexcept { return milliseconds <= 0; }  // <=0 不挂起
    void await_suspend(std::coroutine_handle<> h);  // 注册等待 + 启动定时器
    void await_resume() noexcept {}                 // 无返回值
};
```

**定时器实现（简化版）：**

```
co_await Sleep(100)
     │
     ▼
 await_ready() → false（100 > 0）
     │
     ▼
 await_suspend(h):
   1. sessionId = actor->AllocSession()
   2. actor->StoreWaiting(sessionId, h)
   3. std::thread([=]() {                          // 启动定时器线程
        std::this_thread::sleep_for(100ms);
        ActorMessage wakeup{...};
        wakeup.sessionId = sessionId;
        wakeup.isResponse = true;
        actorSystem->Send(actorId, move(wakeup));  // 发送唤醒消息
      }).detach();
   4. return（协程挂起）
     │
     │  ...100ms 后...
     │
     ▼
 唤醒消息到达邮箱 → OnMessage() → ResumeWaiting() → h.resume()
     │
     ▼
 await_resume()（无操作，协程继续）
```

### 5.4 CoroutineActor — 协程 Actor 基类

```cpp
// CoroutineActor.h
class CoroutineActor : public Actor {
    friend struct CallAwaiter;
    friend struct SleepAwaiter;
public:
    // ---- Actor::OnMessage 重写（final）----
    void OnMessage(ActorMessage& msg) override final;

    // ---- 用户重写的协程处理器 ----
    virtual ActorTask OnCoroutineMessage(ActorMessage msg) = 0;

    // ---- Skynet 风格 API ----
    CallAwaiter Call(uint32_t targetId, ActorMessage&& msg);
    void Respond(const ActorMessage& request, ActorMessage&& response);
    SleepAwaiter Sleep(int ms);

private:
    uint32_t nextSessionId_ = 1;
    std::unordered_map<uint32_t, std::coroutine_handle<>> waitMap_;      // 挂起的协程
    std::unordered_map<uint32_t, ActorMessage>            responseMap_;  // 响应数据
};
```

**OnMessage 分发逻辑：**

```cpp
void CoroutineActor::OnMessage(ActorMessage& msg) {
    if (msg.isResponse && msg.sessionId > 0) {
        // ── Response 消息：恢复等待的协程 ──
        ResumeWaiting(msg.sessionId, std::move(msg));
    } else {
        // ── 新消息：创建新协程 ──
        OnCoroutineMessage(std::move(msg));  // 参数按值传递！
    }
}
```

> **为什么参数按值传递？**
> `OnCoroutineMessage(ActorMessage msg)` 接收值而非引用。因为协程可能在 `co_await` 处挂起，
> 此时 `OnMessage()` 已经返回，原始的 `ActorMessage&` 引用指向的 `optional<ActorMessage>` 
> （在 `ProcessOne()` 中）已经析构。按值传递确保消息数据被 move 到协程帧中，安全存活。

---

## 六、协程生命周期：从消息到达到协程完成

### 6.1 完整流程图

```
  ┌─────────────────────────────────────────────────────────────────┐
  │ Worker 线程 #N                                                   │
  │                                                                  │
  │  workerLoop():                                                   │
  │    readyQueue_.pop() → actorId                                   │
  │    actors_[actorId] → CoroutineActor*                            │
  │    actor->ProcessOne()                                           │
  │      │                                                           │
  │      ▼                                                           │
  │    mailbox_.pop() → ActorMessage                                 │
  │      │                                                           │
  │      ▼                                                           │
  │    OnMessage(msg)                                                │
  │      ├─ msg.isResponse == false:                                 │
  │      │   OnCoroutineMessage(move(msg))                           │
  │      │     │                                                     │
  │      │     ▼ ┌──────────────────────────────────┐                │
  │      │       │ 协程帧（编译器在堆上分配）         │                │
  │      │       │                                    │                │
  │      │       │  auto r = co_await Call(db, query) │                │
  │      │       │    │                               │                │
  │      │       │    ▼                               │                │
  │      │       │  await_suspend(h):                 │                │
  │      │       │    waitMap_[session] = h            │                │
  │      │       │    SendToActor(db, query)           │                │
  │      │       │    return ← 协程挂起               │                │
  │      │       └──────────┬─────────────────────────┘                │
  │      │                  │                                          │
  │      │     ← OnCoroutineMessage 返回（协程已挂起）                  │
  │      │                                                             │
  │      ← OnMessage 返回                                              │
  │    ← ProcessOne 返回                                               │
  │                                                                    │
  │    worker 线程释放，可处理其他 Actor                                 │
  └────────────────────────────────────────────────────────────────────┘

                    │ DB Actor 处理查询...
                    │ RespondToCall() 发回响应
                    ▼

  ┌─────────────────────────────────────────────────────────────────┐
  │ Worker 线程 #M（可能是同一个，也可能是另一个）                     │
  │                                                                  │
  │  workerLoop():                                                   │
  │    readyQueue_.pop() → actorId（CoroutineActor 的 ID）           │
  │    actor->ProcessOne()                                           │
  │      │                                                           │
  │      ▼                                                           │
  │    mailbox_.pop() → ActorMessage{isResponse=true, sessionId=42}  │
  │      │                                                           │
  │      ▼                                                           │
  │    OnMessage(msg)                                                │
  │      ├─ msg.isResponse == true:                                  │
  │      │   ResumeWaiting(sessionId=42, move(msg))                  │
  │      │     │                                                     │
  │      │     ▼                                                     │
  │      │   h = waitMap_[42]                                        │
  │      │   responseMap_[42] = move(msg)                            │
  │      │   h.resume()                                              │
  │      │     │                                                     │
  │      │     ▼ ┌──────────────────────────────────┐                │
  │      │       │ 协程帧（从挂起点恢复）             │                │
  │      │       │                                    │                │
  │      │       │  await_resume():                   │                │
  │      │       │    return responseMap_[42]          │                │
  │      │       │                                    │                │
  │      │       │  // 协程继续执行后续代码...          │                │
  │      │       │  std::cout << r.data;              │                │
  │      │       │                                    │                │
  │      │       │  co_return; ← 协程完成             │                │
  │      │       │  final_suspend → suspend_never     │                │
  │      │       │  → 协程帧自动释放                   │                │
  │      │       └────────────────────────────────────┘                │
  │      │                                                             │
  │      │     ← h.resume() 返回（协程已完成/再次挂起）                 │
  │      ← ResumeWaiting 返回                                         │
  │    ← OnMessage 返回                                                │
  │  ← ProcessOne 返回                                                 │
  └────────────────────────────────────────────────────────────────────┘
```

### 6.2 协程帧中存储了什么

C++20 编译器会自动在协程帧（堆分配）中保存：

```
┌─────────────────────────────────────┐
│         协程帧（编译器生成）          │
├─────────────────────────────────────┤
│ promise_type 实例                    │
│ 挂起点索引（标记从哪个 co_await 恢复）│
│ 函数参数副本：                       │
│   ActorMessage msg  ← 按值传递的消息  │
│ 局部变量：                           │
│   auto r1 (ActorMessage)             │
│   auto r2 (ActorMessage)             │
│   std::string name                   │
│   std::string level                  │
│   ... 所有 co_await 之间的局部变量    │
└─────────────────────────────────────┘
```

> **与 Skynet 的对应关系：**
> Skynet 中每个消息也会创建一个 Lua 协程（`coroutine.create`），
> Lua 协程栈保存局部变量，`coroutine.yield()` 挂起，`coroutine.resume()` 恢复。
> C++20 的协程帧等价于 Lua 协程栈。

---

## 七、Call 机制详解：co_await Call() 的完整流程

### 7.1 场景：GameServiceActor 查询 DatabaseActor

```cpp
// GameServiceActor（CoroutineActor）
ActorTask GameServiceActor::OnCoroutineMessage(ActorMessage msg) {
    // ① co_await Call：发送查询并等待
    auto resp = co_await Call(dbActorId,
        ActorMessage{MsgType::UserMessage, 0, -1, "get:player_name"});
    // ④ 拿到结果
    std::cout << "name = " << resp.data << std::endl;
}

// DatabaseActor（普通 Actor）
void DatabaseActor::OnMessage(ActorMessage& msg) {
    if (msg.data.find("get:") == 0) {
        std::string key = msg.data.substr(4);
        std::string value = db_[key];
        // ③ 响应 Call
        RespondToCall(msg, ActorMessage{MsgType::UserMessage, 0, -1, value});
    }
}
```

### 7.2 消息流转时序图

```
时间线  GameServiceActor          ActorSystem           DatabaseActor
  │    (CoroutineActor)           (调度层)              (普通Actor)
  │
  │    ┌─────────────────┐
  ▼    │ OnCoroutineMsg() │
       │  co_await Call() │
       │  ┌──────────────┐│
       │  │await_suspend()││
       │  │ alloc session ││
       │  │  = 42         ││
       │  │ waitMap_[42]  ││
       │  │  = handle     ││
       │  │ msg.session   ││
       │  │  = 42         ││
       │  │ SendToActor ──┼┼──▶ Send(dbId, msg) ──▶ ┌─────────────────┐
       │  │ (dbId, msg)   ││     PushMessage()       │ OnMessage(msg)  │
       │  │ return        ││     readyQueue_.push     │  msg.sessionId  │
       │  └──────────────┘│                           │   = 42          │
       │  协程挂起         │                           │  msg.sourceId   │
       │  OnMessage 返回   │                           │   = GameSvcId   │
       └──────────────────┘                           │                  │
                                                      │  // 查询 DB      │
       Worker 线程释放                                 │  value = "Alice" │
       可处理其他 Actor                                │                  │
                                                      │  RespondToCall() │
                                                      │  resp.sessionId  │
                                                      │    = 42          │
                                                      │  resp.isResponse │
                                                      │    = true        │
                           ◀── Send(GameSvcId, resp) ─┤  SendToActor()  │
                            PushMessage()              └─────────────────┘
                            readyQueue_.push()
                                    │
                                    ▼
       ┌─────────────────┐
       │ OnMessage(resp)  │
       │  isResponse=true │
       │  sessionId=42    │
       │  ResumeWaiting() │
       │  ┌──────────────┐│
       │  │ h = waitMap_  ││
       │  │      [42]     ││
       │  │ responseMap_  ││
       │  │  [42] = resp  ││
       │  │ h.resume()  ──┼┼─▶ ┌──────────────────┐
       │  │               ││    │  await_resume()   │
       │  │               ││    │  return resp      │
       │  │               ││    │                   │
       │  │               ││    │  // 协程继续执行   │
       │  │               ││    │  resp.data="Alice"│
       │  │               ││    │  cout << name     │
       │  │               ││    │                   │
       │  │               ││    │  co_return        │
       │  │               ││    │  → 协程帧销毁     │
       │  │  ◀────────────┼┼────┘                   │
       │  │ h.resume返回   ││
       │  └──────────────┘│
       │  OnMessage 返回   │
       └──────────────────┘
```

### 7.3 sessionId 的作用

`sessionId` 是 Call/Response 配对的关键：

```
GameServiceActor 同时发出两个 Call：

co_await Call(dbId, "get:name")   →  sessionId = 1
co_await Call(dbId, "get:level")  →  sessionId = 2

DB 响应到达（可能乱序）：
  {data="42", sessionId=2, isResponse=true}  → 恢复 session 2 的协程
  {data="Alice", sessionId=1, isResponse=true}  → 恢复 session 1 的协程
```

> **与 Skynet 对比：**
> Skynet 中 `skynet.call()` 也会分配 session，
> 底层用 `PTYPE_RESPONSE` 类型的消息携带 session 返回。
> 本框架的 `isResponse + sessionId` 等价于 Skynet 的 session 机制。

---

## 八、Sleep 机制详解：co_await Sleep() 的完整流程

```
co_await Sleep(100)
  │
  ▼
await_suspend(h):
  sessionId = AllocSession()           // 比如 = 5
  StoreWaiting(5, h)                   // waitMap_[5] = h
  std::thread([=]() {                  // 启动定时器
    sleep_for(100ms);
    ActorMessage wakeup{...};
    wakeup.sessionId = 5;
    wakeup.isResponse = true;
    actorSystem->Send(actorId, wakeup); // 发给自己
  }).detach();
  return  // 协程挂起
  │
  │  100ms 后...
  │
  ▼
定时器线程：
  Send(actorId, wakeup)
    → PushMessage 到邮箱
    → readyQueue_.push
    → cv_.notify_one()
  │
  ▼
Worker 线程：
  ProcessOne() → OnMessage(wakeup)
    → isResponse=true, sessionId=5
    → ResumeWaiting(5, wakeup)
    → h.resume()
  │
  ▼
协程从 co_await Sleep 处继续执行
```

> **⚠️ 生产环境优化：**
> 当前 Sleep 使用 `std::thread::detach()` 实现定时器，每次 Sleep 创建一个线程。
> 生产环境应替换为：
> - `timerfd_create()` + `epoll`（利用现有 EventLoop）
> - 时间轮（Timer Wheel / Hierarchical Timing Wheel）
> - 最小堆定时器 + 单独的定时器线程

---

## 九、并发协程：同一 Actor 上的多协程交错执行

### 9.1 Skynet 的并发模型

在 Skynet 中，同一个服务可以有多个协程同时挂起：

```lua
-- 消息1 到达 → 创建协程1
-- 协程1: skynet.call(db, ...) → 挂起
-- 消息2 到达 → 创建协程2
-- 协程2: skynet.call(db, ...) → 挂起
-- DB 响应1 到达 → 恢复协程1
-- DB 响应2 到达 → 恢复协程2
```

### 9.2 本框架的并发模型

完全等价：

```
时间  邮箱                    CoroutineActor 内部状态
 │
 │    [msg_A: query_player]   waitMap_ = {}
 │    [msg_B: query_player]
 │    [msg_C: simple]
 │
 ▼    ProcessOne():
      pop msg_A
      OnMessage(msg_A)
        → OnCoroutineMessage(msg_A) → 协程 A 启动
          → co_await Call(db, "get:name")
            → waitMap_[1] = handle_A
            → SendToActor(db, query)
            → 协程 A 挂起
        ← OnMessage 返回

      邮箱: [msg_B, msg_C, ...]
      waitMap_ = {1: handle_A}

      ProcessOne():
      pop msg_B
      OnMessage(msg_B)
        → OnCoroutineMessage(msg_B) → 协程 B 启动
          → co_await Call(db, "get:name")
            → waitMap_[2] = handle_B
            → SendToActor(db, query)
            → 协程 B 挂起
        ← OnMessage 返回

      waitMap_ = {1: handle_A, 2: handle_B}

      ProcessOne():
      pop msg_C
      OnMessage(msg_C)
        → OnCoroutineMessage(msg_C) → 协程 C 启动
          → 无 co_await，直接执行完毕
          → 协程帧销毁
        ← OnMessage 返回

      waitMap_ = {1: handle_A, 2: handle_B}

      ...DB 响应 session=1 到达...

      ProcessOne():
      pop response_1
      OnMessage(response_1)
        → isResponse=true, sessionId=1
        → ResumeWaiting(1)
          → handle_A.resume()
          → 协程 A 继续执行
          → co_await Call(db, "get:level")
            → waitMap_[3] = handle_A
            → 协程 A 再次挂起
        ← OnMessage 返回

      waitMap_ = {2: handle_B, 3: handle_A}

      ...以此类推...
```

**关键点：**
- 同一 Actor 上可以有多个协程**同时挂起**（`waitMap_` 中多个条目）
- 但同一时刻只有**一个协程在运行**（邮箱串行处理保证）
- 不需要任何锁来保护 `waitMap_` 和 `responseMap_`（同一 Actor 的消息处理是串行的）

---

## 十、链式 Call：跨多层 Actor 的协程调用链

### 10.1 三层调用链

```
CallerActor (CoroutineActor)
  │
  │  co_await Call(middleId, "forward:get:treasure")
  │
  ▼
MiddleServiceActor (CoroutineActor)
  │
  │  co_await Call(dbId, "get:treasure")
  │
  ▼
DatabaseActor (普通 Actor)
  │
  │  RespondToCall(msg, {..."treasure_value"})
  │
  ▲
  │  Response: "treasure_value"
  │
MiddleServiceActor
  │  processed = "middle(treasure_value)"
  │  Respond(originalMsg, processed)
  │
  ▲
  │  Response: "middle(treasure_value)"
  │
CallerActor
  │  finalResult = "middle(treasure_value)"
  │  done!
```

### 10.2 消息流转

```
时间线    CallerActor     MiddleService     DatabaseActor     ActorSystem
  │       (协程)          (协程)            (普通)
  │
  │      Call(mid,        
  │       forward:        
  │       get:treasure)   
  │      session=1        
  │      挂起 ──────────▶ 收到消息
  │                       Call(db,
  │                        get:treasure)
  │                       session=2
  │                       挂起 ──────────▶ 收到消息
  │                                        查询: treasure
  │                                        = "gold"
  │                                        RespondToCall
  │                       ◀──────────────── session=2
  │                       恢复                isResponse=true
  │                       result="gold"
  │                       processed=
  │                        "middle(gold)"
  │                       Respond(session=1)
  │      ◀──────────────── session=1
  │      恢复               isResponse=true
  │      finalResult=
  │       "middle(gold)"
  │      done!
```

**关键：每层 Call 都有自己独立的 sessionId，响应沿原路返回。**

---

## 十一、何时用协程 Actor、何时用普通 Actor

### 11.1 核心判断原则

**是否需要 "发送请求并等待结果" 的模式？**

| 场景 | 是否需要协程 | 推荐基类 | 原因 |
|---|---|---|---|
| 网关转发（收到消息直接 SendToActor 给场景服） | ❌ 不需要 | `Actor` | 纯转发，不需要等结果 |
| 聊天广播（收到聊天，SendToActor 给所有玩家） | ❌ 不需要 | `Actor` | fire-and-forget |
| Echo 服务器（收到数据原样回传） | ❌ 不需要 | `Actor` | 直接 SendToNetwork |
| 战斗中介（BattleActor 收到攻击指令，计算伤害） | ❌ 不需要 | `Actor` | 独立计算，不依赖其他 Actor 的结果 |
| 玩家登录（查 DB → 校验 → 初始化 → 响应） | ✅ 需要 | `CoroutineActor` | 多步异步操作，每步依赖上一步的结果 |
| 商店购买（查背包 → 扣钱 → 加物品 → 通知） | ✅ 需要 | `CoroutineActor` | 需要等 DB 响应后才能继续 |
| RPC 代理（收到请求 → 转发 → 等响应 → 回复） | ✅ 需要 | `CoroutineActor` | 经典 Call/Response 模式 |

**简单规则：**
- **只需要 `SendToActor()` / `SendToNetwork()`** → 用普通 `Actor`
- **需要 `co_await Call()` 等待结果** → 用 `CoroutineActor`

### 11.2 网关服的典型场景分析

网关服（GatewayActor）的核心职责是 **消息路由**：

```
客户端 ──TCP──▶ GatewayActor ──SendToActor──▶ SceneActor
                                              ──SendToActor──▶ ChatActor
                                              ──SendToActor──▶ BattleActor
```

**这种纯转发场景不需要协程。** 直接用普通 `Actor` 即可：

```cpp
// GatewayActor — 普通 Actor（不需要协程）
class GatewayActor : public Actor {
public:
    void OnMessage(ActorMessage& msg) override {
        switch (msg.type) {
        case MsgType::Connected:
            // 新连接：创建 PlayerActor，重绑 fd
            break;
            
        case MsgType::NetworkRecv:
            // 解析协议头，确定目标服务
            if (isSceneMsg(msg))
                SendToActor(sceneActorId, std::move(msg));   // 转发给场景服
            else if (isChatMsg(msg))
                SendToActor(chatActorId, std::move(msg));    // 转发给聊天服
            // ✅ 纯转发，不需要等结果
            break;
            
        case MsgType::Disconnected:
            // 通知相关服务
            SendToActor(sceneActorId, {Disconnected...});
            break;
        }
    }
};
```

**在项目已有的 `test_actor_msg.cc` 中就是这么做的：**
[`GatewayActor`](net_actor/test_actor_msg.cc:57) 收到 `NetworkRecv` 后直接 `SendToActor(processorActorId, ...)` 转发。
[`test_actor_msg_chat.cc`](net_actor/test_actor_msg_chat.cc:118) 中的 `GatewayActor` 也是纯转发 + 广播。

### 11.3 什么时候网关也需要协程

如果网关需要 **在转发之前做验证或查询**，就需要协程了：

```cpp
// AuthGatewayActor — 需要协程的网关
class AuthGatewayActor : public CoroutineActor {
public:
    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        if (msg.type == MsgType::NetworkRecv) {
            // 先查询认证服务，确认令牌有效
            auto authResult = co_await Call(authServiceId,
                ActorMessage{..., "verify_token:" + msg.data});
            
            if (authResult.data == "ok") {
                // 认证通过，才转发给场景服
                SendToActor(sceneActorId, std::move(msg));
            } else {
                // 认证失败，断开连接
                SendToNetwork(msg.fd, "auth_failed", 11);
            }
        }
    }
};
```

### 11.4 框架的双轨设计

本框架 **同时支持两种 Actor**，它们可以自由混合：

```
╔═══════════════════════════════════════════════════════════════╗
║                        ActorSystem                            ║
║                                                               ║
║  ┌──────────────┐  ┌──────────────┐  ┌───────────────────┐   ║
║  │ GatewayActor │  │ SceneActor   │  │ PlayerService     │   ║
║  │ (普通 Actor) │  │ (普通 Actor) │  │ (CoroutineActor)  │   ║
║  │              │  │              │  │                    │   ║
║  │ OnMessage()  │  │ OnMessage()  │  │ OnCoroutineMsg()  │   ║
║  │ {            │  │ {            │  │ {                  │   ║
║  │   // 转发    │  │   // 处理    │  │   auto r =        │   ║
║  │   SendTo     │  │   移动/战斗  │  │     co_await      │   ║
║  │   Actor()    │  │   ...        │  │     Call(db,...);  │   ║
║  │ }            │  │ }            │  │   co_await         │   ║
║  │              │  │              │  │     Sleep(100);    │   ║
║  └──────────────┘  └──────────────┘  └───────────────────┘   ║
║       │                  ▲                   │   ▲            ║
║       │   SendToActor    │                   │   │            ║
║       └──────────────────┘    co_await Call   │   │ Response   ║
║                               ───────────────┘   │            ║
║                                                   │            ║
║  ┌──────────────┐                                 │            ║
║  │ DatabaseActor│                                 │            ║
║  │ (普通 Actor) │ ◀── Call ──────────────────────┘            ║
║  │              │                                              ║
║  │ OnMessage()  │  ── RespondToCall ──▶ PlayerService          ║
║  │ {查询DB}     │                                              ║
║  └──────────────┘                                              ║
╚═══════════════════════════════════════════════════════════════╝

通信规则：
  普通 Actor → 普通 Actor:    SendToActor()        ✅
  普通 Actor → 协程 Actor:    SendToActor()        ✅
  协程 Actor → 普通 Actor:    co_await Call()      ✅（对端用 RespondToCall 响应）
  协程 Actor → 普通 Actor:    SendToActor()        ✅（fire-and-forget）
  协程 Actor → 协程 Actor:    co_await Call()      ✅（对端用 Respond 响应）
```

### 11.5 Skynet 中的对应关系

Skynet 也是同样的设计哲学：

| Skynet 服务 | 通信方式 | 对应本框架 |
|---|---|---|
| watchdog（网关） | `skynet.send(agent, ...)` 转发 | `Actor` + `SendToActor()` |
| agent（连接代理） | `skynet.send()` 转发到业务服 | `Actor` + `SendToActor()` |
| login（登录服务） | `skynet.call(db, ...) + skynet.call(auth, ...)` | `CoroutineActor` + `co_await Call()` |
| gated（需要鉴权的网关） | `skynet.call(auth, ...)` 验证后转发 | `CoroutineActor` + `co_await Call()` |

**Skynet 中也不是所有服务都需要协程。** `skynet.send()` 对应普通 Actor 的 `SendToActor()`，只有需要 `skynet.call()` 等待结果时才用到协程。

### 11.6 性能考量

| | 普通 Actor | 协程 Actor |
|---|---|---|
| 每条消息开销 | 无额外开销 | 堆分配协程帧（~数百字节） |
| 适用场景 | 高频转发/广播 | 低频但复杂的业务流程 |
| 内存 | 仅 Actor 对象 | Actor 对象 + 挂起的协程帧 |
| 代码复杂度 | switch-case | 顺序的 co_await |

**典型游戏服务器的 Actor 类型分布：**

```
网关服 GatewayActor      → 普通 Actor（高频转发，不需要等结果）
场景服 SceneActor         → 普通 Actor（AOI 广播、移动同步）
聊天服 ChatActor          → 普通 Actor（广播，不需要等结果）
战斗服 BattleActor        → 普通 Actor（计算伤害，广播结果）
玩家服 PlayerServiceActor → 协程 Actor（登录、购买、任务 需要查 DB）
组队服 TeamServiceActor   → 协程 Actor（匹配队友 需要查询多方状态）
邮件服 MailServiceActor   → 协程 Actor（发邮件+附件 需要事务操作）
DB 代理 DatabaseActor     → 普通 Actor（执行查询，RespondToCall 返回结果）
```

> **结论：大部分服务用普通 `Actor` 就够了。只有需要 "发请求等结果" 模式的服务才用 `CoroutineActor`。** 框架的双轨设计允许两者自由混合使用。

---

## 十二、线程安全分析

### 12.1 Actor 模型的串行保证

原有 Actor 模型保证：**同一个 Actor 的 `OnMessage()` 不会被并发调用。**

这是因为：
1. `ActorSystem::workerLoop()` 从 `readyQueue_` 取到 actorId 后，调用 `ProcessOne()`
2. `ProcessOne()` 中调用 `OnMessage()`
3. `scheduled_` 原子标志 + CAS 保证同一 Actor 不会被多个 worker 同时处理

### 12.2 协程 Actor 的线程安全

```
CoroutineActor 内部数据结构：
  waitMap_      — std::unordered_map<uint32_t, coroutine_handle>
  responseMap_  — std::unordered_map<uint32_t, ActorMessage>
  nextSessionId_ — uint32_t
```

**这些数据结构不需要加锁！** 原因：

| 操作 | 访问时机 | 线程 |
|---|---|---|
| `AllocSession()` | 在 `await_suspend()` 中调用，此时在 `OnMessage()` 内 | worker 线程 |
| `StoreWaiting()` | 同上 | worker 线程 |
| `ResumeWaiting()` | 在 `OnMessage()` 处理 Response 时调用 | worker 线程 |
| `responseMap_` 读写 | `ResumeWaiting` 写入，`await_resume` 读取 | 同一个 `OnMessage()` 调用栈内 |

由于同一 Actor 的 `OnMessage()` 是串行调用的，所有对 `waitMap_`、`responseMap_` 的访问都在同一时间线上，**无竞态条件**。

### 12.3 SleepAwaiter 的线程安全

`SleepAwaiter::await_suspend()` 中的 `std::thread::detach()` 创建的定时器线程，其唯一操作是调用 `ActorSystem::Send()`。而 `Send()` 是线程安全的（通过 SpinLock 保护 actors_ 映射，PushMessage 使用 SpinLockQueue）。

---

## 十三、与原有 Actor 模型的兼容性

### 13.1 向后兼容

所有现有的普通 Actor（`EchoServerActor`、`EchoClientActor`、`BattleActor` 等）**无需任何修改**。

| 组件 | 影响 |
|---|---|
| `Actor` 基类 | 仅新增 `RespondToCall()` 方法（不影响现有子类） |
| `ActorMessage` | 新增的 `sessionId` 和 `isResponse` 默认值为 `0` 和 `false`，不影响现有消息 |
| `ActorSystem` | 无修改 |
| `EventLoop` | 无修改 |
| 现有测试 | 全部兼容（test_actor_battle、test_actor_msg 等） |

### 13.2 混合使用

**协程 Actor 可以与普通 Actor 自由通信：**

```
CoroutineActor              普通 Actor
     │                          │
     │  co_await Call(id, msg)  │
     │  ───────────────────▶    │
     │                          │  OnMessage(msg)
     │                          │  msg.sessionId = 42
     │                          │  RespondToCall(msg, resp)
     │  ◀───────────────────    │
     │  resp.data = "result"    │
```

**普通 Actor 也可以被协程 Actor Call：**
只需在 `OnMessage()` 中调用 `RespondToCall(msg, response)` 即可。

---

## 十四、测试用例说明

### test_coroutine_actor.cc — 5 个测试

| 测试 | 场景 | 验证点 |
|---|---|---|
| **Test1: BasicCall** | GameService `co_await Call(db, "get:name")` + `co_await Call(db, "get:level")` | DB 被查询 2 次，结果正确组合为 "Alice:lv42" |
| **Test2: Sleep** | GameService `co_await Sleep(100)` | 实际耗时 >= 80ms（允许误差） |
| **Test3: ConcurrentCoroutines** | 同时发 3 条消息（2 个 query + 1 个 simple），产生 3 个协程 | 3 个协程全部完成，DB 查询 4 次（2×2） |
| **Test4: ChainCall** | Caller → Middle → DB 三层链式 `co_await Call` | 最终结果 "middle(treasure)"，Middle 处理 1 次 |
| **Test5: Integration** | 3 个玩家同时登录，每个走 Call→Sleep→Send 流程 | 3 个登录全部完成，DB 中 last_login 记录存在 |

### 构建与运行

```bash
# 一键构建 & 运行
cd net_actor
chmod +x run_test_coroutine.sh
./run_test_coroutine.sh

# 或手动构建
make -f Makefile.coroutine
./test_coroutine_actor
```

**要求：** GCC 11+ 或 Clang 14+（C++20 coroutine 支持）

---

## 十五、生产环境优化建议

### 15.1 定时器优化

当前 `SleepAwaiter` 使用 `std::thread::detach()` 创建独立线程，不适合高频使用。

**推荐方案：**

```
方案1: timerfd + epoll（利用现有 EventLoop）
  - timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)
  - EventLoop::AddEvent(timerfd, EPOLLIN, callback)
  - callback 中发送 wakeup 消息

方案2: 时间轮（Timer Wheel）
  - 单独的定时器线程
  - O(1) 添加/取消定时器
  - 适合大量并发 Sleep
```

### 15.2 协程帧池化

频繁创建/销毁协程帧会产生堆分配开销。可以在 `promise_type` 中重载 `operator new/delete` 实现池化：

```cpp
struct ActorTask::promise_type {
    void* operator new(size_t size) {
        return CoroutineFramePool::Alloc(size);
    }
    void operator delete(void* ptr) {
        CoroutineFramePool::Free(ptr);
    }
};
```

### 15.3 Call 超时

当前 `co_await Call()` 没有超时机制。可以扩展为：

```cpp
// 带超时的 Call
auto resp = co_await CallWithTimeout(targetId, msg, 5000/*ms*/);
if (resp.type == MsgType::None) {
    // 超时处理
}
```

实现方式：在 `await_suspend` 中同时注册一个定时器，超时后发送特殊的 timeout 响应。

### 15.4 协程取消

当 Actor 被 UnregisterActor 时，应清理所有挂起的协程：

```cpp
CoroutineActor::~CoroutineActor() {
    for (auto& [session, handle] : waitMap_) {
        handle.destroy();  // 销毁挂起的协程帧
    }
    waitMap_.clear();
}
```

### 15.5 错误传播

当前 `unhandled_exception()` 直接 `std::terminate()`。可以改为将异常存储在 promise 中，在适当时机重新抛出或记录日志。

---

## 附录 A：架构完善性分析 — 现有框架的不足与改进方向

本节从成熟 Actor 框架（Skynet、Akka、Orleans）的角度，全面审视当前框架在**同步/异步支持之外**还缺少哪些关键能力。

### A.1 总览：现有能力 vs 缺失能力

```
╔═══════════════════════════════════════════════════════════════════╗
║                     能力矩阵                                      ║
╠═══════════════════╦═══════╦═══════════════════════════════════════╣
║ 能力              ║ 现状  ║ 说明                                  ║
╠═══════════════════╬═══════╬═══════════════════════════════════════╣
║ Actor 注册/注销   ║  ✅   ║ RegisterActor / UnregisterActor       ║
║ 邮箱串行处理      ║  ✅   ║ SpinLockQueue + scheduled_ CAS        ║
║ 工作线程池        ║  ✅   ║ workerLoop + readyQueue               ║
║ 网络I/O集成       ║  ✅   ║ EventLoop + epoll + fd-to-Actor映射    ║
║ Actor间消息通信   ║  ✅   ║ SendToActor / SendByFd                ║
║ 协程 Call/Response║  ✅   ║ CoroutineActor + CallAwaiter          ║
║ 协程 Sleep        ║  ✅   ║ SleepAwaiter（需优化定时器实现）       ║
╠═══════════════════╬═══════╬═══════════════════════════════════════╣
║ 异常保护/监督     ║  ❌   ║ OnMessage 抛异常会导致 worker 退出     ║
║ 定时器/周期任务   ║  ⚠️   ║ 仅 CoroutineActor 有 Sleep            ║
║ Actor 命名/发现   ║  ❌   ║ 仅靠 uint32_t actorId，无名字查找     ║
║ 邮箱容量控制      ║  ❌   ║ 无界队列，可能 OOM                    ║
║ Actor 监控/Link   ║  ❌   ║ 无法感知其他 Actor 的死亡              ║
║ 优雅关停          ║  ⚠️   ║ Stop() 不排空邮箱，消息可能丢失        ║
║ 消息序列化        ║  ❌   ║ 仅 string data，无结构化协议           ║
║ 跨进程/集群       ║  ❌   ║ 单进程，无远程 Actor                   ║
║ 指标监控          ║  ❌   ║ 无邮箱大小、处理延迟等指标             ║
║ 优先级消息        ║  ❌   ║ 单 FIFO 队列，无优先级                 ║
║ Actor 热更新      ║  ❌   ║ 无运行时替换行为                       ║
╚═══════════════════╩═══════╩═══════════════════════════════════════╝
```

---

### A.2 异常保护与监督（Supervision）— 重要度：⭐⭐⭐⭐⭐

**现状问题：**

`workerLoop()` 中调用 `actor->ProcessOne()` → `OnMessage()`。
如果 `OnMessage()` 抛出异常（如 `std::stoi` 解析失败、空指针访问等），异常会传播到 `workerLoop()`，**导致该 worker 线程退出**。随着异常累积，所有 worker 线程可能全部退出，Actor 系统实质停止运行。

```
当前代码（workerLoop 无异常保护）：
  while (running_) {
      auto optId = readyQueue_.pop();
      ...
      while (actor->ProcessOne() && ++processed < 64) {}  // ← 这里可能抛异常
      ...
  }
  // 异常从这里逃逸，std::thread 终止
```

**Skynet 的做法：**
Skynet 用 `pcall`（protected call）包裹每条消息的处理，异常被捕获并记录日志，服务继续运行。

**改进建议：**

```cpp
// 方案1：workerLoop 中 try-catch
while (running_) {
    ...
    try {
        while (actor->ProcessOne() && ++processed < 64) {}
    } catch (const std::exception& e) {
        std::cerr << "[ActorSystem] Actor " << actorId
                  << " OnMessage exception: " << e.what() << std::endl;
        // 可选：通知监督者、重启 Actor、或标记为 dead
    }
    ...
}

// 方案2：监督者模式（Akka 风格）
class SupervisorActor : public Actor {
    void OnChildFailure(uint32_t childId, const std::exception& e) {
        // 策略：Restart / Stop / Escalate
        if (shouldRestart(childId)) {
            system_->RestartActor(childId);
        }
    }
};
```

---

### A.3 定时器与周期任务（Timer / Tick）— 重要度：⭐⭐⭐⭐⭐

**现状问题：**

- 只有 `CoroutineActor` 的 `co_await Sleep()` 支持定时
- 普通 `Actor` 没有任何定时能力
- 没有周期性 Tick 机制（游戏服务器核心需求：帧同步、AOI 更新、Buff 倒计时等）
- `SleepAwaiter` 每次创建一个 `std::thread`，性能差

**Skynet 的做法：**
- `skynet.timeout(n, func)` — 注册一次性定时回调
- `skynet.fork(func)` + `while true do skynet.sleep(n) ... end` — 实现周期任务
- 底层使用时间轮（timer wheel），O(1) 添加/触发

**改进建议：**

```cpp
// 在 ActorSystem 中添加定时器管理
class ActorSystem {
public:
    // 一次性定时器：n 毫秒后给 actorId 发消息
    void SetTimeout(uint32_t actorId, int ms, ActorMessage&& msg);
    // 周期性定时器：每 intervalMs 毫秒给 actorId 发消息
    uint32_t SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg);
    // 取消定时器
    void CancelTimer(uint32_t timerId);

private:
    // 方案1: timerfd + epoll（利用现有 EventLoop）
    // 方案2: 最小堆 + 专用定时器线程
    // 方案3: 时间轮（适合大量定时器的游戏场景）
};

// 普通 Actor 使用定时器
class SceneActor : public Actor {
    void OnMessage(ActorMessage& msg) override {
        if (msg.type == MsgType::Connected) {
            // 注册 50ms 周期 Tick
            GetSystem()->SetInterval(GetActorId(), 50,
                ActorMessage{MsgType::UserMessage, 0, -1, "tick"});
        }
        if (msg.data == "tick") {
            UpdateAOI();
            UpdateBuffs();
        }
    }
};
```

---

### A.4 Actor 命名与发现（Naming / Discovery）— 重要度：⭐⭐⭐⭐

**现状问题：**

当前只能通过 `uint32_t actorId` 查找 Actor。如果一个 Actor 需要找到 "数据库服务"、"场景服务" 等，必须在初始化时手动传入 ID。

```cpp
// 当前做法：手动传 ID
playerActor->dbActorId = dbId;           // 写死
playerActor->sceneActorId = sceneId;     // 写死
```

**Skynet 的做法：**
```lua
local db = skynet.localname(".db")       -- 按名字查找
local scene = skynet.queryservice("scene") -- 查询全局服务
```

**改进建议：**

```cpp
class ActorSystem {
public:
    // 按名字注册
    void RegisterName(const std::string& name, uint32_t actorId);
    // 按名字查找
    uint32_t FindActor(const std::string& name);
    // 按名字发消息
    void SendByName(const std::string& name, ActorMessage&& msg);

private:
    std::unordered_map<std::string, uint32_t> nameToActor_;
};

// 使用
uint32_t dbId = sys.RegisterActor(make_unique<DatabaseActor>());
sys.RegisterName("db", dbId);

// 任何 Actor 中
uint32_t db = GetSystem()->FindActor("db");
SendToActor(db, {...});
```

---

### A.5 邮箱容量控制（Backpressure）— 重要度：⭐⭐⭐⭐

**现状问题：**

`SpinLockQueue` 是**无界队列**。如果生产者速度远大于消费者（比如 I/O 线程疯狂往某个 Actor 投递网络数据），邮箱会无限增长直到 OOM。

**场景举例：**
1000 个客户端同时发送大量数据 → 全部路由到同一个 `GatewayActor` → 邮箱堆积 → 内存耗尽

**改进建议：**

```cpp
// 方案1：有界队列 + 拒绝策略
template<typename T>
class BoundedQueue {
    size_t maxSize_ = 65536;
    bool push(T&& value) {
        if (size() >= maxSize_) return false;  // 满了，拒绝
        // 或者：阻塞等待、丢弃最旧消息、触发背压
        ...
    }
};

// 方案2：邮箱水位线告警
void Actor::PushMessage(ActorMessage&& msg) {
    mailbox_.push(std::move(msg));
    if (mailbox_.size() > HIGH_WATER_MARK) {
        std::cerr << "[WARN] Actor " << actorId_
                  << " mailbox overflow: " << mailbox_.size() << std::endl;
    }
}
```

---

### A.6 Actor 监控与 Link（Watch / Monitor）— 重要度：⭐⭐⭐

**现状问题：**

一个 Actor 无法知道另一个 Actor 是否"死亡"（被 Unregister 或异常退出）。

**场景举例：**
`PlayerActor` 正在与 `BattleActor` 战斗，`BattleActor` 被意外注销 → `PlayerActor` 的 `co_await Call(battleId, ...)` 永远等不到响应 → **协程泄漏**。

**Skynet 的做法：**
```lua
skynet.monitor("exit", function(name) ... end)
```

**改进建议：**

```cpp
class ActorSystem {
public:
    // 监控：当 targetId 被注销时，通知 watcherId
    void Watch(uint32_t watcherId, uint32_t targetId);
    void Unwatch(uint32_t watcherId, uint32_t targetId);

    void UnregisterActor(uint32_t actorId) {
        // 在注销前，通知所有 watcher
        for (uint32_t wid : watchers_[actorId]) {
            Send(wid, ActorMessage{MsgType::ActorDown, actorId, -1, ""});
        }
        // ... 原有注销逻辑
    }
};
```

这也解决了 **Call 超时 / 目标不存在** 的问题：
```cpp
// CoroutineActor 处理 ActorDown 消息
void OnMessage(ActorMessage& msg) override {
    if (msg.type == MsgType::ActorDown) {
        // 清理所有等待该 Actor 响应的协程
        CancelWaitingForActor(msg.sourceId);
    }
    // ...
}
```

---

### A.7 优雅关停（Graceful Shutdown）— 重要度：⭐⭐⭐

**现状问题：**

`ActorSystem::Stop()` 设置 `running_ = false`，worker 线程退出循环。但此时：
- 邮箱中可能还有未处理的消息 → **消息丢失**
- 正在执行的 `OnMessage()` 可能被中断（thread::join 等待当前 ProcessOne 完成，但后续消息不再处理）
- 挂起的协程（`waitMap_` 中）不会被 resume → **协程帧泄漏**

**改进建议：**

```cpp
void ActorSystem::Stop() {
    // Phase 1: 停止接收新消息
    accepting_ = false;

    // Phase 2: 排空所有 Actor 的邮箱
    for (auto& [id, actor] : actors_) {
        while (!actor->IsMailboxEmpty()) {
            actor->ProcessOne();
        }
    }

    // Phase 3: 销毁协程（CoroutineActor 的 waitMap_）
    for (auto& [id, actor] : actors_) {
        if (auto* coro = dynamic_cast<CoroutineActor*>(actor.get())) {
            coro->DestroyAllCoroutines();  // destroy 所有挂起的 coroutine_handle
        }
    }

    // Phase 4: 停止 worker 线程
    running_ = false;
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}
```

---

### A.8 消息类型系统（Message Protocol）— 重要度：⭐⭐⭐

**现状问题：**

所有消息都通过 `std::string data` 携带数据。业务层需要手动解析字符串：

```cpp
// 到处都是字符串解析
if (msg.data.find("get:") == 0) {
    std::string key = msg.data.substr(4);  // 脆弱！
}
if (msg.data.find("broadcast:") == 0) {
    std::string content = msg.data.substr(10);  // 容易出错
}
```

这种方式：
- 无类型安全（编译期不检查）
- 性能差（字符串拷贝 + 查找）
- 容易出 bug（偏移量写错）

**改进建议：**

```cpp
// 方案1：使用 std::any / std::variant
struct ActorMessage {
    MsgType type;
    uint32_t sourceId;
    int fd;
    std::any payload;  // 替代 std::string data
};

// 发送
sys.Send(dbId, ActorMessage{MsgType::UserMessage, 0, -1,
    DBQuery{"player", "get", "name"}});  // 结构化数据

// 接收
auto& query = std::any_cast<DBQuery&>(msg.payload);

// 方案2：保留 string，但使用 protobuf/flatbuffers 序列化
// 方案3：二进制协议（header + body）
struct ActorMessage {
    MsgType type;
    uint32_t sourceId;
    int fd;
    uint32_t protoId;               // 协议号
    std::vector<uint8_t> payload;   // 二进制数据
};
```

---

### A.9 指标监控（Metrics）— 重要度：⭐⭐⭐

**现状问题：**

没有任何运行时指标。线上出问题时无法定位：
- 哪个 Actor 的邮箱积压？
- 哪个 Actor 处理消息最慢？
- worker 线程利用率如何？

**改进建议：**

```cpp
struct ActorMetrics {
    std::atomic<uint64_t> totalMsgProcessed{0};  // 累计处理消息数
    std::atomic<uint64_t> totalProcessTimeUs{0};  // 累计处理耗时（微秒）
    std::atomic<uint32_t> currentMailboxSize{0};  // 当前邮箱大小
    std::atomic<uint64_t> maxMailboxSize{0};       // 历史最大邮箱大小
};

// 在 ProcessOne 中统计
bool Actor::ProcessOne() {
    auto opt = mailbox_.pop();
    if (!opt) return false;

    auto start = std::chrono::steady_clock::now();
    OnMessage(*opt);
    auto elapsed = std::chrono::steady_clock::now() - start;

    metrics_.totalMsgProcessed.fetch_add(1);
    metrics_.totalProcessTimeUs.fetch_add(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    return true;
}

// 定期打印或暴露给监控系统
```

---

### A.10 优先级消息 — 重要度：⭐⭐

**现状问题：**

单 FIFO 队列，所有消息同等优先级。系统控制消息（如 Stop、配置更新）必须排在大量业务消息之后。

**Skynet 的做法：**
Skynet 有两个消息队列（一般消息和控制消息），控制消息优先处理。

**改进建议：**

```cpp
class Actor {
    SpinLockQueue<ActorMessage> normalMailbox_;   // 普通消息
    SpinLockQueue<ActorMessage> priorityMailbox_;  // 优先消息

    bool ProcessOne() {
        // 优先处理高优先级消息
        auto opt = priorityMailbox_.pop();
        if (!opt) opt = normalMailbox_.pop();
        if (!opt) return false;
        OnMessage(*opt);
        return true;
    }
};
```

---

### A.11 跨进程 / 集群（Cluster）— 重要度：⭐⭐

**现状问题：**

所有 Actor 在同一进程内。无法：
- 将不同服务部署到不同机器
- 水平扩展（多个场景服分布在多台机器）

**Skynet 的做法：**
`skynet.cluster.call(node, addr, ...)` — 跨节点 RPC

**改进方向（长期）：**

```
                   ┌───────────────────┐
                   │   Cluster Proxy   │ ← 透明代理
                   │  (ActorSystem)    │
                   └─────┬───────┬─────┘
                         │       │
              TCP/UDP    │       │    TCP/UDP
                         ▼       ▼
          ┌──────────────┐     ┌──────────────┐
          │   Node A     │     │   Node B     │
          │ ActorSystem  │     │ ActorSystem  │
          │ Scene1,Scene2│     │ Scene3,Scene4│
          └──────────────┘     └──────────────┘

  SendToActor(remoteActorId, msg)
    → 序列化 msg → 通过 TCP 发到远程节点
    → 远程节点反序列化 → 投递到目标 Actor 邮箱
```

---

### A.12 改进优先级建议

根据**游戏服务器实际需求**排列优先级：

```
优先级    改进项                          原因
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
P0       异常保护 (A.2)                  不做就是线上事故，worker 线程会死
P0       定时器/Tick (A.3)               游戏服务器核心需求，当前只有协程Sleep
P1       Actor 命名 (A.4)               易用性大幅提升，代码更清晰
P1       邮箱容量控制 (A.5)              防止 OOM，保护线上稳定性
P1       优雅关停 (A.7)                  防止消息丢失和协程泄漏
P2       Actor 监控/Link (A.6)          解决 Call 目标不存在时的协程泄漏
P2       消息类型系统 (A.8)              提升开发效率和运行时性能
P2       指标监控 (A.9)                  线上问题定位能力
P3       优先级消息 (A.10)              控制消息需要优先处理
P3       跨进程集群 (A.11)              后期扩展需求
P3       Actor 热更新                    运行时不停服更新（可选）
```

> **结论：当前框架的核心通信模型（邮箱串行 + 工作线程池 + 协程 Call/Response）是完善的。
> 最需要补充的是 P0 级别的异常保护和定时器支持——前者关系到系统稳定性，后者是游戏服务器的基础设施。**
