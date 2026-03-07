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
- [十六、Actor 监控/Link 机制](#十六actor-监控link-机制)
- [十七、消息类型系统 — std::any payload](#十七消息类型系统--stdany-payload)
- [十八、指标监控 — ActorMetrics](#十八指标监控--actormetrics)
- [十九、优先级消息 — 双队列邮箱](#十九优先级消息--双队列邮箱)
- [二十、跨进程集群基础 — ClusterProxy](#二十跨进程集群基础--clusterproxy)
- [二十一、高级特性测试用例说明](#二十一高级特性测试用例说明)
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

## 十六、Actor 监控/Link 机制

### 16.1 设计目标与 Skynet/Erlang 对标

Actor 监控/Link 是成熟 Actor 框架的核心能力，解决以下问题：
- **Actor 异常退出感知**：Actor A 监控 Actor B，B 退出时 A 收到通知
- **防止协程泄漏**：`co_await Call(targetId, ...)` 的目标被注销时，调用方能感知并清理
- **周期性健康检查**：系统级监控，遍历所有 Actor 的邮箱积压、调度状态

| Erlang/OTP | Skynet | 本框架 C++20 | 功能说明 |
|---|---|---|---|
| `erlang:monitor(process, Pid)` | `skynet.monitor("exit", func)` | `LinkTo(targetId)` | 监控目标 Actor |
| `erlang:demonitor(Ref)` | — | `UnlinkFrom(targetId)` | 取消监控 |
| `{'DOWN', Ref, process, Pid, Reason}` | 回调通知 | `ActorDown` 消息 | 退出通知 |
| — | — | `StartMonitor(actorId, ms)` | 周期性健康检查 |
| — | — | `CollectActorStats()` | 收集全局 Actor 统计 |

### 16.2 核心数据结构

```
  ActorSystem
  ┌──────────────────────────────────────────────────┐
  │  linkMap_: unordered_map<uint32_t, set<uint32_t>>│
  │                                                   │
  │  targetId=5 → {watcherId=1, watcherId=3}         │  Actor 1 和 3 都在监控 Actor 5
  │  targetId=7 → {watcherId=1}                      │  Actor 1 还监控了 Actor 7
  │                                                   │
  │  linkLock_: SpinLock                              │  保护 linkMap_ 的自旋锁
  └──────────────────────────────────────────────────┘
```

### 16.3 Link/Unlink 流程

```
  Actor 1 (Supervisor)              ActorSystem                    Actor 5 (Worker)
       │                                │                               │
       │  LinkTo(5)                     │                               │
       │──────────────────────────────▷│                               │
       │                                │ linkMap_[5].insert(1)         │
       │                                │                               │
       │                                │         ... 运行中 ...         │
       │                                │                               │
       │                                │◁─── UnregisterActor(5) ──────│
       │                                │                               │
       │                                │ 1. notifyActorDown(5, "unregistered")
       │                                │    ├─ lock: watchers = linkMap_[5] → {1}
       │                                │    ├─ unlock
       │                                │    └─ Send(1, ActorDown{sourceId=5,
       │                                │         data="5:unregistered"})
       │◁─ ActorDown 消息 ─────────────│                               │
       │                                │ 2. 清理名字映射                │
       │  OnMessage():                  │ 3. 清理 linkMap 条目          │
       │    msg.type == ActorDown       │ 4. actors_.erase(5)           │
       │    msg.sourceId == 5           │                               │
       │    msg.data == "5:unregistered"│                               │
       │    → 处理子Actor退出           │                               │
```

### 16.4 锁安全设计

**关键设计：锁外发消息，避免死锁**

```cpp
void ActorSystem::notifyActorDown(uint32_t targetId, const std::string& reason)
{
    // ① 在 linkLock_ 下取出 watchers（快速）
    std::set<uint32_t> watchers;
    {
        LockGuard<SpinLock> lock(linkLock_);
        auto it = linkMap_.find(targetId);
        if (it != linkMap_.end()) {
            watchers = std::move(it->second);
            linkMap_.erase(it);
        }
    }
    // ② 在锁外发送消息（避免 linkLock_ → actorsLock_ 嵌套死锁）
    for (uint32_t watcherId : watchers) {
        Send(watcherId, ActorMessage{MsgType::ActorDown, targetId, -1, ...});
    }
}
```

**锁顺序约定（防止死锁）：**

```
获取顺序: linkLock_ → 释放 → actorsLock_（通过 Send()）
         nameLock_ → 释放 → actorsLock_（通过 Send()）
绝不能: 持有 actorsLock_ 的同时获取 linkLock_ 或 nameLock_
```

### 16.5 UnregisterActor 完整清理流程

当一个 Actor 被注销时，需要清理三方面数据：

```
UnregisterActor(actorId=5):
  │
  ├─ ① notifyActorDown(5, "unregistered")
  │     → 向所有 watcher 发送 ActorDown 消息
  │     → 清理 linkMap_[5]（作为 target 的条目）
  │
  ├─ ② 清理名字映射
  │     → nameLock_ 下：actorToName_.erase(5), nameToActor_.erase(name)
  │
  ├─ ③ 清理 Link 注册表（作为 watcher 的角色）
  │     → linkLock_ 下：遍历所有 linkMap_ 条目，移除 watcherId=5
  │     → 确保 Actor 5 不会在其他 Actor 退出时收到无效通知
  │
  └─ ④ 删除 Actor
        → actorsLock_ 下：actors_.erase(5)
```

### 16.6 周期性健康监控

使用 TimerManager 的 `SetInterval` 实现周期性遍历所有 Actor 状态：

```
  ┌───────────────────────────────────────────────────────────────┐
  │                    监控架构                                     │
  │                                                                 │
  │   TimerManager ──SetInterval──▷ MonitorActor (每 N 秒)          │
  │                                      │                          │
  │                        收到 __monitor_tick__ 消息                │
  │                                      │                          │
  │                                      ▼                          │
  │                        CollectActorStats()                      │
  │                        ┌──────────────────────────┐             │
  │                        │ Phase 1 (actorsLock_):   │             │
  │                        │   遍历 actors_           │             │
  │                        │   收集 id, mailboxSize,  │             │
  │                        │         scheduled        │             │
  │                        ├──────────────────────────┤             │
  │                        │ Phase 2 (nameLock_):     │             │
  │                        │   填充 name (命名映射)    │             │
  │                        └──────────────────────────┘             │
  │                                      │                          │
  │                                      ▼                          │
  │                        返回 vector<ActorStat>                   │
  │                        MonitorActor 输出/上报统计                │
  └───────────────────────────────────────────────────────────────┘
```

**ActorStat 统计快照结构：**

```cpp
struct ActorStat {
    uint32_t actorId;        // Actor ID
    std::string name;        // 命名（未命名为空）
    size_t mailboxSize;      // 当前邮箱积压量
    bool scheduled;          // 是否在就绪队列中等待处理
};
```

### 16.7 使用示例

#### 示例1：Supervisor 模式（子 Actor 退出时自动重启）

```cpp
class SupervisorActor : public Actor {
    uint32_t childId_ = 0;

    void StartChild() {
        auto child = std::make_unique<WorkerActor>();
        childId_ = GetSystem()->RegisterActor(std::move(child));
        LinkTo(childId_);  // 监控子 Actor
        std::cout << "Started child actor id=" << childId_ << std::endl;
    }

    void OnMessage(ActorMessage& msg) override {
        switch (msg.type) {
        case MsgType::ActorDown:
            // 子 Actor 退出，自动重启
            std::cout << "Child actor " << msg.sourceId << " down: "
                      << msg.data << ", restarting..." << std::endl;
            StartChild();
            break;
        case MsgType::UserMessage:
            if (msg.data == "start") {
                StartChild();
            }
            break;
        default:
            break;
        }
    }
};
```

#### 示例2：周期性健康检查 Actor

```cpp
class HealthMonitorActor : public Actor {
    void OnMessage(ActorMessage& msg) override {
        if (msg.data == "__monitor_tick__") {
            auto stats = GetSystem()->CollectActorStats();
            std::cout << "=== Health Report ===" << std::endl;
            std::cout << "Total actors: " << stats.size() << std::endl;
            for (auto& s : stats) {
                // 检测邮箱积压过高的 Actor
                if (s.mailboxSize > 100) {
                    std::cerr << "[WARN] Actor " << s.actorId
                              << " (name=" << s.name << ")"
                              << " mailbox=" << s.mailboxSize
                              << " OVERLOADED!" << std::endl;
                }
            }
        }
    }
};

// 启动监控：每 5 秒检查一次
uint32_t monitorId = sys.RegisterActor(std::make_unique<HealthMonitorActor>());
uint64_t timerId = sys.StartMonitor(monitorId, 5000);
// 停止监控
sys.CancelTimer(timerId);
```

#### 示例3：协程 Actor 中使用 Link 防止 Call 泄漏

```cpp
class GameServiceActor : public CoroutineActor {
    ActorTask OnCoroutineMessage(ActorMessage msg) override {
        if (msg.data == "query_battle") {
            uint32_t battleId = FindActorByName("battle_service");
            LinkTo(battleId);  // 监控 BattleActor

            // 如果 BattleActor 在此期间退出，
            // 本 Actor 会收到 ActorDown 消息
            auto result = co_await Call(battleId,
                ActorMessage{MsgType::UserMessage, 0, -1, "get_status"});

            UnlinkFrom(battleId);
            std::cout << "Battle status: " << result.data << std::endl;
        }
    }

    // ActorDown 消息会通过 OnMessage 分发，
    // 在 CoroutineActor 中可以扩展处理：
    // 清理所有等待该 Actor 响应的协程句柄
};
```

### 16.8 涉及文件修改清单

| 文件 | 修改内容 | 说明 |
|---|---|---|
| `Message.h` | `MsgType` 枚举新增 `ActorDown` | 被 Link 的 Actor 退出时发送给 watcher 的消息类型 |
| `ActorSystem.h` | 新增 `ActorStat` 结构体 | 监控统计快照 |
| `ActorSystem.h` | 新增 `LinkActor()` / `UnlinkActor()` | 注册/取消监控关系 |
| `ActorSystem.h` | 新增 `StartMonitor()` | 启动周期性健康检查定时器 |
| `ActorSystem.h` | 新增 `CollectActorStats()` | 收集所有 Actor 统计快照 |
| `ActorSystem.h` | 新增 `GetActorCount()` | 获取当前 Actor 数量 |
| `ActorSystem.h` | 新增 `linkMap_` + `linkLock_` | Link 注册表存储 |
| `ActorSystem.cc` | 实现 `LinkActor()` / `UnlinkActor()` | 操作 linkMap_ |
| `ActorSystem.cc` | 新增 `notifyActorDown()` 私有方法 | 锁外发送 ActorDown 消息 |
| `ActorSystem.cc` | 修改 `UnregisterActor()` | 退出前通知 watcher + 清理 linkMap |
| `ActorSystem.cc` | 实现 `CollectActorStats()` | 分段加锁收集统计 |
| `ActorSystem.cc` | 修改 `Stop()` | Phase 5 增加 linkMap_ 清理 |
| `Actor.h` | 新增 `LinkTo()` / `UnlinkFrom()` protected 方法 | Actor 基类辅助方法 |
| `Actor.cc` | 实现 `LinkTo()` / `UnlinkFrom()` | 委托给 `system_->LinkActor()` |

### 16.9 线程安全分析

| 操作 | 涉及的锁 | 安全性说明 |
|---|---|---|
| `LinkActor()` | `linkLock_` | 单锁操作，安全 |
| `UnlinkActor()` | `linkLock_` | 单锁操作，安全 |
| `notifyActorDown()` | `linkLock_` → 释放 → `Send()` 内部获取 `actorsLock_` | 分段加锁，无嵌套，安全 |
| `UnregisterActor()` | `notifyActorDown()` → `nameLock_` → `linkLock_` → `actorsLock_` | 顺序获取且每段独立释放，安全 |
| `CollectActorStats()` | `actorsLock_` → 释放 → `nameLock_` | 分段加锁，无嵌套，安全 |
| `StartMonitor()` | 通过 `SetInterval()` → `TimerManager` | TimerManager 有自己的锁，与 Actor 锁无关 |

---

## 附录 A：架构完善性分析 — 现有框架的不足与改进方向

本节从成熟 Actor 框架（Skynet、Akka、Orleans）的角度，全面审视当前框架在**同步/异步支持之外**还缺少哪些关键能力。

### A.1 总览：现有能力 vs 缺失能力

```
╔════════════════════════════════════════════════════════════════════════════════════╗
║                               能力矩阵（全部已实现 ✅）                             ║
╠═══════════════════╦═══════╦══════════════════════════════════════╦════════════════╣
║ 能力              ║ 状态  ║ 说明                                  ║ 详细章节      ║
╠═══════════════════╬═══════╬══════════════════════════════════════╬════════════════╣
║ Actor 注册/注销   ║  ✅   ║ RegisterActor / UnregisterActor       ║ 三            ║
║ 邮箱串行处理      ║  ✅   ║ SpinLockQueue + scheduled_ CAS        ║ 四            ║
║ 工作线程池        ║  ✅   ║ workerLoop + readyQueue               ║ 五            ║
║ 网络I/O集成       ║  ✅   ║ EventLoop + epoll + fd-to-Actor映射    ║ 四            ║
║ Actor间消息通信   ║  ✅   ║ SendToActor / SendByFd                ║ 四            ║
║ 协程 Call/Response║  ✅   ║ CoroutineActor + CallAwaiter          ║ 七~十         ║
║ 协程 Sleep        ║  ✅   ║ SleepAwaiter + TimerManager           ║ 十二          ║
╠═══════════════════╬═══════╬══════════════════════════════════════╬════════════════╣
║ 异常保护/监督     ║  ✅   ║ workerLoop try-catch 包裹 ProcessOne  ║ A.2, 十三     ║
║ 定时器/周期任务   ║  ✅   ║ TimerManager + SetTimeout/SetInterval ║ A.3, 十二     ║
║ Actor 命名/发现   ║  ✅   ║ RegisterName/FindActor/SendByName     ║ A.4, 十三     ║
║ 邮箱容量控制      ║  ✅   ║ SetMailboxHighWaterMark + 告警日志     ║ A.5, 十三     ║
║ Actor 监控/Link   ║  ✅   ║ LinkActor/ActorDown/StartMonitor      ║ A.6, 十六     ║
║ 优雅关停          ║  ✅   ║ 5阶段 Stop + DestroyAllCoroutines     ║ A.7, 十三     ║
║ 消息类型系统      ║  ✅   ║ std::any payload + 模板辅助方法       ║ A.8, 十七     ║
║ 指标监控          ║  ✅   ║ ActorMetrics + ProcessOne 自动计时     ║ A.9, 十八     ║
║ 优先级消息        ║  ✅   ║ 双队列邮箱 + SendPriority             ║ A.10, 十九    ║
║ 跨进程/集群       ║  ✅   ║ ClusterProxy + TcpClusterTransport    ║ A.11, 二十    ║
╚═══════════════════╩═══════╩══════════════════════════════════════╩════════════════╝
```

---

### A.2 异常保护与监督（Supervision）— 重要度：⭐⭐⭐⭐⭐ ✅ 已实现

**问题描述：**

`workerLoop()` 中调用 `actor->ProcessOne()` → `OnMessage()`。
如果 `OnMessage()` 抛出异常（如 `std::stoi` 解析失败、空指针访问等），异常会传播到 `workerLoop()`，导致该 worker 线程退出。

**实现方案：** 在 `workerLoop()` 中用 try-catch 包裹 `ProcessOne()` 调用

**修改文件：** `ActorSystem.cc` — `workerLoop()`

```cpp
// 实际实现代码
try {
    while (actor->ProcessOne() && ++processed < 64) {}
} catch (const std::exception& e) {
    std::cerr << "[ActorSystem] Actor " << actorId
              << " OnMessage exception: " << e.what() << std::endl;
} catch (...) {
    std::cerr << "[ActorSystem] Actor " << actorId
              << " OnMessage unknown exception!" << std::endl;
}
```

**效果：** 单个 Actor 的 `OnMessage()` 抛异常不会影响 worker 线程，该 Actor 跳过当前消息继续处理后续消息。

---

### A.3 定时器与周期任务（Timer / Tick）— 重要度：⭐⭐⭐⭐⭐ ✅ 已实现

**问题描述：**

- 只有 `CoroutineActor` 的 `co_await Sleep()` 支持定时
- 普通 `Actor` 没有任何定时能力
- `SleepAwaiter` 每次创建一个 `std::thread`，性能差

**实现方案：** 最小堆 + 专用定时器线程（`TimerManager`）

**新增文件：** `Timer.h` / `Timer.cc`
**修改文件：** `ActorSystem.h/cc`（集成定时器）、`Actor.h/cc`（辅助方法）、`Coroutine.h`（`SleepAwaiter` 改用 `TimerManager`）

**核心 API：**

```cpp
// ActorSystem 定时器接口
void SetTimeout(uint32_t actorId, int ms, ActorMessage&& msg);   // 一次性
uint64_t SetInterval(uint32_t actorId, int intervalMs, ActorMessage&& msg); // 周期性
void CancelTimer(uint64_t timerId);

// Actor 辅助方法
void SetTimeout(int ms, ActorMessage&& msg);          // 给自己设超时
uint64_t SetInterval(int intervalMs, ActorMessage&& msg); // 给自己设周期
void CancelTimer(uint64_t timerId);
```

**TimerManager 设计：**
- 数据结构：`std::priority_queue`（最小堆），按到期时间排序
- 定时器条目：`shared_ptr<TimerEntry>`，含 `cancelled` 标记支持延迟删除
- 周期定时器：到期后自动创建下一个 entry 重新入堆
- 线程：独立 timer 线程，`cv.wait_until` 等待最近到期
- 触发：到期后向目标 Actor 的邮箱投递消息

**SleepAwaiter 优化：** 改用 `TimerManager::AddTimer()` 替代 `std::thread`，到期后 resume 协程

---

### A.4 Actor 命名与发现（Naming / Discovery）— 重要度：⭐⭐⭐⭐ ✅ 已实现

**问题描述：**

当前只能通过 `uint32_t actorId` 查找 Actor，无法按名字查找"数据库服务"、"场景服务"等。

**实现方案：** 在 `ActorSystem` 中维护 `nameToActor_` 映射表

**修改文件：** `ActorSystem.h/cc`（`RegisterName`/`FindActor`/`SendByName`）、`Actor.h/cc`（辅助方法）

**核心 API：**

```cpp
// ActorSystem 命名接口
void RegisterName(const std::string& name, uint32_t actorId);  // 注册名字
uint32_t FindActor(const std::string& name);                   // 按名查找
void SendByName(const std::string& name, ActorMessage&& msg);  // 按名发消息

// Actor 辅助方法
void RegisterName(const std::string& name);  // 给自己注册名字
```

**实现细节：**
- `nameToActor_` 由 `SpinLock nameLock_` 保护
- `FindActor()` 找不到返回 0
- `SendByName()` 内部调用 `FindActor()` + `Send()`
- `UnregisterActor()` 时自动清理 name 映射（反向查找后删除）

---

### A.5 邮箱容量控制（Backpressure）— 重要度：⭐⭐⭐⭐ ✅ 已实现

**问题描述：**

`SpinLockQueue` 是无界队列，生产者速度大于消费者时邮箱可能无限增长导致 OOM。

**实现方案：** 邮箱水位线告警（High Water Mark）

**修改文件：** `Actor.h`（`mailboxHighWaterMark_` 成员 + `SetMailboxHighWaterMark()`）、`Actor.cc`（`PushMessage()` 中检测）

**核心实现：**

```cpp
// Actor.h
size_t mailboxHighWaterMark_ = 10000;  // 默认高水位线
void SetMailboxHighWaterMark(size_t mark) { mailboxHighWaterMark_ = mark; }

// Actor.cc — PushMessage() 中
size_t currentSize = GetMailboxSize();
if (mailboxHighWaterMark_ > 0 && currentSize > mailboxHighWaterMark_) {
    std::cerr << "[WARN] Actor " << actorId_
              << " mailbox high water mark! size=" << currentSize
              << ", limit=" << mailboxHighWaterMark_ << std::endl;
}
```

**设计选择：** 采用告警模式而非拒绝模式，因为丢弃消息在 Actor 模型中会导致逻辑错误（如 Call 的 response 被丢弃会导致协程永久挂起）。告警 + 指标监控（A.9）组合使用可及时发现问题。

---

### A.6 Actor 监控与 Link（Watch / Monitor）— 重要度：⭐⭐⭐ ✅ 已实现

> **已实现：** 详见 [十六、Actor 监控/Link 机制](#十六actor-监控link-机制)

**实现内容：**
- `LinkActor(watcherId, targetId)` / `UnlinkActor()` — 注册/取消监控
- `ActorDown` 消息类型 — target 退出时自动通知所有 watcher
- `StartMonitor(reportActorId, intervalMs)` — 周期性健康检查
- `CollectActorStats()` — 收集所有 Actor 的统计快照
- `Actor::LinkTo()` / `Actor::UnlinkFrom()` — Actor 基类辅助方法

**涉及文件修改：**
- `Message.h` — 新增 `MsgType::ActorDown`
- `ActorSystem.h/cc` — Link 注册表、健康监控、统计收集
- `Actor.h/cc` — Link 辅助方法

---

### A.7 优雅关停（Graceful Shutdown）— 重要度：⭐⭐⭐ ✅ 已实现

**问题描述：**

`ActorSystem::Stop()` 简单设置 `running_ = false`，导致邮箱消息丢失和协程帧泄漏。

**实现方案：** 5 阶段有序关停

**修改文件：** `ActorSystem.cc`（`Stop()` 重写）、`CoroutineActor.h/cc`（`DestroyAllCoroutines()`）

**5 阶段 Stop() 流程：**

```
Phase 1: 停止定时器线程（阻止新的定时消息进入）
  └─ timerManager_.Stop()

Phase 2: 停止 worker 线程（等待正在执行的 ProcessOne 完成）
  └─ running_ = false → cv_.notify_all() → join all workers

Phase 3: 排空邮箱 + 销毁协程（防止消息丢失和协程泄漏）
  └─ 遍历所有 Actor:
     ├─ actor->ProcessOne() 直到邮箱空
     └─ dynamic_cast<CoroutineActor*> → DestroyAllCoroutines()

Phase 4: 日志记录（统计排空的消息数）
  └─ 输出 "drained N messages for M actors"

Phase 5: 清理资源
  └─ workers_.clear()
```

**CoroutineActor::DestroyAllCoroutines()：** 遍历 `waitMap_`，对每个挂起的 `coroutine_handle` 调用 `destroy()`，防止协程帧内存泄漏。

---

### A.8 消息类型系统（Message Protocol）— 重要度：⭐⭐⭐ ✅ 已实现

**问题描述：**

所有消息通过 `std::string data` 携带数据，需手动字符串解析，无类型安全。

**实现方案：** `std::any payload` + 模板辅助方法（保留 `data` 字段兼容旧代码）

**修改文件：** `Message.h`

**核心 API：**

```cpp
struct ActorMessage {
    // ... 原有字段保留 ...
    std::any payload;                          // 类型安全 payload

    template<typename T> void SetPayload(T&& value);       // 设置
    template<typename T> const T* GetPayload() const;      // 安全获取（返回nullptr表示类型不匹配）
    template<typename T> const T& GetPayloadRef() const;   // 引用获取（类型不匹配抛 bad_any_cast）
    bool HasPayload() const;                               // 是否有值
    template<typename T> bool IsPayloadType() const;       // 类型检查
};
```

**使用示例：**

```cpp
// 发送结构化数据（零字符串编解码）
struct PlayerInfo { std::string name; int level; int hp; };
ActorMessage msg{MsgType::UserMessage, 0, -1, ""};
msg.SetPayload(PlayerInfo{"Warrior", 50, 1000});

// 接收：类型安全
if (msg.IsPayloadType<PlayerInfo>()) {
    const auto& info = msg.GetPayloadRef<PlayerInfo>();
}
```

> 详细设计见 [十七、消息类型系统](#十七消息类型系统--stdany-payload)

---

### A.9 指标监控（Metrics）— 重要度：⭐⭐⭐ ✅ 已实现

**问题描述：**

没有运行时指标，无法定位邮箱积压、处理延迟等线上问题。

**实现方案：** `ActorMetrics` 结构 + `ProcessOne()` 自动计时 + `CollectActorStats()` 扩展

**修改文件：** `Actor.h`（`ActorMetrics` 结构体 + `metrics_` 成员）、`Actor.cc`（`ProcessOne()` 中计时）、`ActorSystem.h/cc`（`ActorStat` 扩展 + `CollectActorStats()` 读取指标）

**ActorMetrics 字段：**

| 字段 | 类型 | 说明 | 更新位置 |
|------|------|------|----------|
| `totalMsgProcessed` | `atomic<uint64_t>` | 累计处理消息数 | `ProcessOne()` |
| `totalProcessTimeUs` | `atomic<uint64_t>` | 累计处理耗时（微秒） | `ProcessOne()` |
| `maxProcessTimeUs` | `atomic<uint64_t>` | 单条消息最大耗时 | `ProcessOne()` CAS |
| `maxMailboxSize` | `atomic<uint64_t>` | 历史最大邮箱大小 | `PushMessage()` CAS |
| `totalPriorityMsgProcessed` | `atomic<uint64_t>` | 累计优先级消息数 | `ProcessOne()` |

**CAS 更新最大值（无锁）：**

```cpp
uint64_t prevMax = metrics_.maxProcessTimeUs.load();
while (elapsedUs > prevMax) {
    if (metrics_.maxProcessTimeUs.compare_exchange_weak(prevMax, elapsedUs)) break;
}
```

**查看指标：** `CollectActorStats()` 返回 `std::vector<ActorStat>`，包含每个 Actor 的完整指标快照。

> 详细设计见 [十八、指标监控](#十八指标监控--actormetrics)

---

### A.10 优先级消息 — 重要度：⭐⭐ ✅ 已实现

**问题描述：**

单 FIFO 队列，系统控制消息（Stop、配置更新）被大量业务消息阻塞。

**实现方案：** 双队列邮箱（`priorityMailbox_` + `normalMailbox_`），`ProcessOne()` 优先出队优先级队列

**修改文件：** `Actor.h`（双队列成员）、`Actor.cc`（`PushMessage`/`ProcessOne`/`GetMailboxSize`/`IsMailboxEmpty`）、`ActorSystem.h/cc`（`SendPriority()`）

**核心逻辑：**

```cpp
// PushMessage — 按 priority 标志路由
void Actor::PushMessage(ActorMessage&& msg) {
    if (msg.priority) { priorityMailbox_.push(std::move(msg)); }
    else              { normalMailbox_.push(std::move(msg));   }
}

// ProcessOne — 优先级队列优先
bool Actor::ProcessOne() {
    auto opt = priorityMailbox_.pop();      // 先取优先级
    if (!opt) opt = normalMailbox_.pop();   // 空则取普通
    ...
}
```

**新增 API：**
- `ActorSystem::SendPriority(actorId, msg)` — 设置 `msg.priority=true` 后发送
- `Actor::SendPriorityToActor(targetId, msg)` — Actor 层辅助方法

**向后兼容：** 原有 `SendToActor()`/`Send()` 默认 `priority=false`，旧代码无需修改。

> 详细设计见 [十九、优先级消息](#十九优先级消息--双队列邮箱)

---

### A.11 跨进程 / 集群（Cluster）— 重要度：⭐⭐ ✅ 已实现（含 TCP 传输）

**问题描述：**

所有 Actor 在同一进程内，无法跨节点部署和水平扩展。

**实现方案：** `ClusterProxy` 代理 Actor + `IClusterTransport` 传输接口 + `TcpClusterTransport` TCP 实现

**新增文件：** `ClusterProxy.h` / `ClusterProxy.cc`

**核心组件：**

| 组件 | 说明 |
|------|------|
| `RemoteActorRef` | 远程 Actor 引用（nodeId + actorId 或 actorName） |
| `IClusterTransport` | 传输层接口（`SendPacket()`），可实现为 TCP/UDP/共享内存 |
| `LoopbackTransport` | 同进程回环传输（单元测试用，handler 直接调用） |
| `ClusterProxy` | 本地代理 Actor：收到消息 → 构造 `ClusterPacket` → 通过 transport 发送 |
| `ClusterReceiver` | 接收端（LoopbackTransport 用）：`ClusterPacket` → `ActorMessage` → 投递到本地 ActorSystem |
| `ClusterPacketCodec` | 二进制序列化/反序列化（长度前缀线协议，支持 TCP 流重组） |
| `ClusterGatewayActor` | 集群 TCP I/O 处理 Actor（per-fd 接收缓冲区，握手协议，数据包路由） |
| `TcpClusterTransport` | 真实 TCP 传输层：`Listen()`/`ConnectToNode()`/`SendPacket()`，基于 EventLoop+Acceptor+Connector |

**两种寻址方式：**
- 按 ID：`RemoteActorRef("nodeB", 42)` → 直接投递到 nodeB 的 Actor#42
- 按名字：`RemoteActorRef("nodeB", "db_service")` → nodeB 端 `FindActor("db_service")` 查找后投递

**TCP 传输特性：** 长度前缀二进制协议、握手自动识别节点、TCP 流重组（半包/粘包处理）、双向通信、SpinLock 保护的 fd↔nodeId 映射。

> 详细设计见 [二十、跨进程集群基础](#二十跨进程集群基础--clusterproxy)

---

### A.12 改进优先级建议

根据**游戏服务器实际需求**排列优先级：

```
优先级    改进项                          原因                              状态
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
P0       异常保护 (A.2)                  不做就是线上事故，worker 线程会死    ✅ 已实现
P0       定时器/Tick (A.3)               游戏服务器核心需求                   ✅ 已实现
P1       Actor 命名 (A.4)               易用性大幅提升，代码更清晰            ✅ 已实现
P1       邮箱容量控制 (A.5)              防止 OOM，保护线上稳定性              ✅ 已实现
P1       优雅关停 (A.7)                  防止消息丢失和协程泄漏               ✅ 已实现
P2       Actor 监控/Link (A.6)          解决 Call 目标不存在时的协程泄漏      ✅ 已实现
P2       消息类型系统 (A.8)              std::any payload + 模板辅助方法      ✅ 已实现
P2       指标监控 (A.9)                  ActorMetrics + ProcessOne 耗时统计    ✅ 已实现
P3       优先级消息 (A.10)              双队列邮箱 + SendPriority            ✅ 已实现
P3       跨进程集群 (A.11)              ClusterProxy + TcpClusterTransport   ✅ 已实现
```

> **当前状态：全部 10 项改进已全部实现！🎉**
> 框架已具备完整的生产级能力：
> - **P0 核心**：异常保护、定时器系统
> - **P1 易用**：Actor 命名/发现、邮箱容量控制、优雅关停
> - **P2 监控**：Actor 监控/Link、消息类型系统（std::any payload）、指标监控（ActorMetrics）
> - **P3 扩展**：优先级消息（双队列邮箱）、跨进程集群（ClusterProxy + TCP 传输层）
>
> 各功能的详细设计文档见对应章节（十七～二十一）。

---

## 十七、消息类型系统 — std::any payload

### 17.1 动机

原有 `ActorMessage` 仅有 `std::string data` 字段，所有消息数据必须序列化为字符串。在游戏服务器中，传递结构化数据（如 `PlayerInfo`、`DamageEvent`）需要频繁进行字符串编解码，既低效又容易出错。

Skynet 中通过 `skynet.pack()`/`skynet.unpack()` 来处理，但 C++ 有更好的方式：**`std::any`**。

### 17.2 设计原则

1. **向后兼容**：`std::string data` 字段保留，旧代码无需修改
2. **类型安全**：编译期不强制类型匹配，但运行期可检查类型
3. **零额外开销**：不使用 `payload` 时，`std::any` 为空状态，不分配堆内存
4. **API 简洁**：提供模板辅助方法，一行代码设置/获取

### 17.3 ActorMessage 扩展

**修改文件：** `Message.h`

```cpp
struct ActorMessage {
    // ... 原有字段 ...
    std::string data;          // 向后兼容，字符串格式消息
    
    // [P2] A.8 新增
    std::any payload;          // 类型安全 payload（可选）

    // 设置 payload（完美转发）
    template<typename T>
    void SetPayload(T&& value) {
        payload = std::forward<T>(value);
    }

    // 获取 payload（安全版，返回 nullptr 表示类型不匹配或为空）
    template<typename T>
    const T* GetPayload() const {
        return std::any_cast<T>(&payload);
    }

    // 获取 payload（抛异常版本，用于确定类型正确的场景）
    template<typename T>
    const T& GetPayloadRef() const {
        return std::any_cast<const T&>(payload);
    }

    // 检查 payload 是否有值
    bool HasPayload() const { return payload.has_value(); }

    // 检查 payload 是否为指定类型
    template<typename T>
    bool IsPayloadType() const {
        return payload.type() == typeid(T);
    }
};
```

### 17.4 使用示例

```cpp
// 定义业务结构体
struct PlayerInfo {
    std::string name;
    int level;
    int hp;
};

// 发送方：设置 payload
ActorMessage msg{MsgType::UserMessage, 0, -1, "player_info"};
msg.SetPayload(PlayerInfo{"Warrior", 50, 1000});
SendToActor(targetId, std::move(msg));

// 接收方：获取 payload
void OnMessage(ActorMessage& msg) override {
    if (msg.IsPayloadType<PlayerInfo>()) {
        const auto* info = msg.GetPayload<PlayerInfo>();
        // info->name, info->level, info->hp 安全访问
    }
}
```

### 17.5 `data` vs `payload` 选择指南

| 场景 | 推荐 | 理由 |
|------|------|------|
| 简单文本消息（如 "chat:hello"） | `data` | 字符串直观，无需定义结构体 |
| 跨进程通信（需序列化） | `data` | `std::any` 无法跨进程传输 |
| 进程内结构化数据传递 | `payload` | 零拷贝，类型安全，无编解码开销 |
| 包含大二进制数据 | `payload` | 避免 string→any→string 转换 |

### 17.6 性能对比

```
场景：传递 PlayerInfo{name="test", level=50, hp=1000}

字符串方式：序列化 "test:50:1000" → 传递 → 解析 Split(':') → stoi()
  开销：~2次内存分配 + 字符串拼接 + 解析

std::any 方式：msg.SetPayload(PlayerInfo{...}) → move 到 any → GetPayload<>()
  开销：1次小对象优化内存（SBO，<= 3个指针大小的结构体零堆分配）
```

---

## 十八、指标监控 — ActorMetrics

### 18.1 动机

线上运行时，需要回答以下问题：
- 哪个 Actor 消息处理最慢？
- 哪个 Actor 邮箱积压最严重？
- 系统整体消息吞吐量是多少？

没有指标数据，这些问题只能靠猜测。Skynet 通过 `skynet.stat("mqlen")` / `skynet.stat("message")` 提供类似能力。

### 18.2 ActorMetrics 结构

**修改文件：** `Actor.h`

```cpp
struct ActorMetrics {
    std::atomic<uint64_t> totalMsgProcessed{0};        // 累计处理消息数
    std::atomic<uint64_t> totalProcessTimeUs{0};        // 累计处理耗时（微秒）
    std::atomic<uint64_t> maxProcessTimeUs{0};          // 单条消息最大处理耗时
    std::atomic<uint64_t> maxMailboxSize{0};             // 历史最大邮箱大小
    std::atomic<uint64_t> totalPriorityMsgProcessed{0}; // 累计优先级消息数

    void Reset();                      // 重置所有计数器
    uint64_t AvgProcessTimeUs() const; // 平均处理耗时
};
```

所有字段使用 `std::atomic`，保证多线程下的读取安全（worker 线程写，监控线程读）。

### 18.3 自动统计：ProcessOne 中的计时

**修改文件：** `Actor.cc`

```cpp
bool Actor::ProcessOne()
{
    // [P3] 优先级队列优先处理
    auto opt = priorityMailbox_.pop();
    bool isPriority = opt.has_value();
    if (!opt) { opt = normalMailbox_.pop(); }
    if (!opt) { return false; }

    // 计时开始
    auto start = std::chrono::steady_clock::now();
    OnMessage(*opt);
    auto elapsed = std::chrono::steady_clock::now() - start;
    uint64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();

    // 更新指标（全部原子操作，无锁）
    metrics_.totalMsgProcessed.fetch_add(1);
    metrics_.totalProcessTimeUs.fetch_add(elapsedUs);
    if (isPriority) {
        metrics_.totalPriorityMsgProcessed.fetch_add(1);
    }

    // CAS 更新最大处理耗时
    uint64_t prevMax = metrics_.maxProcessTimeUs.load();
    while (elapsedUs > prevMax) {
        if (metrics_.maxProcessTimeUs.compare_exchange_weak(prevMax, elapsedUs)) break;
    }

    return true;
}
```

### 18.4 邮箱大小历史峰值追踪

在 `PushMessage()` 中更新：

```cpp
void Actor::PushMessage(ActorMessage&& msg)
{
    // 路由到对应队列 ...

    // 更新历史最大邮箱大小
    size_t currentSize = GetMailboxSize();
    uint64_t prevMax = metrics_.maxMailboxSize.load();
    while (currentSize > prevMax) {
        if (metrics_.maxMailboxSize.compare_exchange_weak(prevMax, currentSize)) break;
    }
}
```

### 18.5 CollectActorStats 扩展

**修改文件：** `ActorSystem.cc`

`CollectActorStats()` 现在返回包含完整指标数据的 `ActorStat`：

```cpp
struct ActorStat {
    uint32_t actorId;
    std::string name;
    size_t mailboxSize;
    bool scheduled;
    // 以下为新增指标字段
    uint64_t totalMsgProcessed;
    uint64_t totalProcessTimeUs;
    uint64_t maxProcessTimeUs;
    uint64_t avgProcessTimeUs;
    uint64_t maxMailboxSize;
    uint64_t totalPriorityMsgProcessed;
};
```

### 18.6 监控使用示例

```cpp
// 方式一：直接调用 CollectActorStats()
auto stats = sys.CollectActorStats();
for (const auto& s : stats) {
    std::cout << "Actor " << s.actorId
              << " processed=" << s.totalMsgProcessed
              << " avgTime=" << s.avgProcessTimeUs << "us"
              << " maxTime=" << s.maxProcessTimeUs << "us"
              << " mailbox=" << s.mailboxSize
              << " maxMailbox=" << s.maxMailboxSize << std::endl;
}

// 方式二：通过 StartMonitor 定期自动采集
uint64_t monitorTimerId = sys.StartMonitor(monitorActorId, 5000); // 每5秒
```

---

## 十九、优先级消息 — 双队列邮箱

### 19.1 动机

单 FIFO 队列中，所有消息同等优先级。系统控制消息（如 Stop 信号、配置热更新、心跳超时）必须排在大量业务消息之后处理，可能导致响应延迟。

Skynet 有两个消息队列（一般消息和控制消息），控制消息优先处理。

### 19.2 双队列架构

```
                PushMessage(msg)
                      │
                      ▼
              ┌── msg.priority? ──┐
              │ true              │ false
              ▼                   ▼
    ┌─────────────────┐  ┌─────────────────┐
    │ priorityMailbox_│  │ normalMailbox_   │
    │   [msg1][msg2]  │  │   [msg3][msg4]  │
    └────────┬────────┘  └────────┬────────┘
             │                     │
             └──────── ProcessOne ─┘
                      │
              先检查 priorityMailbox_
              若空，再检查 normalMailbox_
```

### 19.3 关键代码

**Actor.h** — 双队列成员：

```cpp
class Actor {
private:
    SpinLockQueue<ActorMessage> priorityMailbox_;   // 高优先级
    SpinLockQueue<ActorMessage> normalMailbox_;     // 普通
};
```

**Actor.cc** — 路由逻辑：

```cpp
void Actor::PushMessage(ActorMessage&& msg) {
    if (msg.priority) {
        priorityMailbox_.push(std::move(msg));
    } else {
        normalMailbox_.push(std::move(msg));
    }
}

bool Actor::ProcessOne() {
    auto opt = priorityMailbox_.pop();    // 优先取高优先级
    if (!opt) opt = normalMailbox_.pop(); // 空则取普通
    if (!opt) return false;
    OnMessage(*opt);
    return true;
}
```

**ActorSystem** — 优先级发送：

```cpp
void ActorSystem::SendPriority(uint32_t actorId, ActorMessage&& msg) {
    msg.priority = true;
    Send(actorId, std::move(msg));
}
```

**Actor** — 辅助方法：

```cpp
void Actor::SendPriorityToActor(uint32_t targetId, ActorMessage&& msg) {
    msg.sourceId = actorId_;
    msg.priority = true;
    system_->Send(targetId, std::move(msg));
}
```

### 19.4 CoroutineActor 兼容性

`CoroutineActor` 继承自 `Actor`，其 `ProcessOne()` 调用链：
1. `CoroutineActor::ProcessOne()` → 检查是否为协程响应
2. 底层使用 `Actor::PushMessage()` 和邮箱队列

由于 `CoroutineActor` 通过公开的 `ProcessOne()`、`PushMessage()` 等方法间接操作邮箱，双队列改造对协程 Actor 完全透明。

### 19.5 使用场景

| 场景 | 发送方式 | 说明 |
|------|----------|------|
| 普通业务消息 | `SendToActor()` | 默认 `priority=false` |
| 系统停止信号 | `SendPriorityToActor()` | 优先于积压的业务消息处理 |
| 配置热更新通知 | `sys.SendPriority()` | 确保快速响应 |
| 心跳超时检测 | `SendPriorityToActor()` | 不被业务洪峰阻塞 |

---

## 二十、跨进程集群基础 — ClusterProxy

### 20.1 动机

所有 Actor 在同一进程内，无法：
- 将不同服务部署到不同机器
- 水平扩展（多个场景服分布在多台机器）

Skynet 通过 `skynet.cluster.call(node, addr, ...)` 提供跨节点 RPC。

### 20.2 架构设计

```
  ┌─────── Node A (本地) ───────┐     ┌─────── Node B (远程) ───────┐
  │                              │     │                              │
  │  PlayerActor                 │     │  DBServiceActor              │
  │    │ SendToActor(proxyId)    │     │    ← OnMessage() 处理       │
  │    ▼                         │     │                              │
  │  ClusterProxy(nodeB, dbSvc)  │     │  ClusterReceiver             │
  │    │ OnMessage()             │     │    ← OnPacketReceived()      │
  │    │ → 构造 ClusterPacket    │     │    → sys.SendByName(name)   │
  │    │ → transport.SendPacket()│     │    (仅名字路由)              │
  │    ▼                         │     │                              │
  │  IClusterTransport           │ ──→ │  IClusterTransport           │
  │  (TCP/UDP/Loopback)          │     │  (TCP/UDP/Loopback)          │
  └──────────────────────────────┘     └──────────────────────────────┘
```

### 20.3 核心组件

**新增文件：** `ClusterProxy.h` / `ClusterProxy.cc`

#### 20.3.1 RemoteActorRef — 远程 Actor 引用

```cpp
struct RemoteActorRef {
    std::string nodeId;        // 远程节点标识
    std::string remoteName;    // 远程 Actor 名字（唯一寻址方式）

    RemoteActorRef(const std::string& node, const std::string& name);
    bool IsValid() const;
    std::string ToString() const;
};
```

**仅支持名字寻址**：`RemoteActorRef("nodeB", "db_service")` → nodeB 通过 `FindActor("db_service")` 查找

> **为什么移除了按 ID 寻址？**
>
> 每个进程独立分配 `actorId`（从 1 递增），NodeA 的 `actorId=3` ≠ NodeB 的 `actorId=3`。
> 跨进程传输 `actorId` 毫无意义，必须使用通过 `RegisterName()` 注册的全局唯一名字。

#### 20.3.2 IClusterTransport — 传输层接口

```cpp
class IClusterTransport {
public:
    virtual bool SendPacket(const ClusterPacket& packet) = 0;
    virtual void SetLocalNodeId(const std::string& nodeId) = 0;
    virtual std::string GetLocalNodeId() const = 0;
    virtual std::vector<ClusterNode> GetKnownNodes() const { return {}; }
};
```

接口抽象，可实现为 TCP、UDP、共享内存等。

#### 20.3.3 LoopbackTransport — 本地回环（测试用）

```cpp
class LoopbackTransport : public IClusterTransport {
    using PacketHandler = std::function<void(const ClusterPacket&)>;

    void RegisterRemoteHandler(const std::string& nodeId, PacketHandler handler);
    bool SendPacket(const ClusterPacket& packet) override;
};
```

同进程内模拟两个节点通信，用于单元测试。

#### 20.3.4 ClusterProxy — 本地代理 Actor

```cpp
class ClusterProxy : public Actor {
public:
    ClusterProxy(const RemoteActorRef& remote, IClusterTransport* transport);

    void OnMessage(ActorMessage& msg) override {
        // 构造 ClusterPacket（纯名字路由）
        ClusterPacket packet;
        packet.sourceNodeId = transport_->GetLocalNodeId();
        packet.targetNodeId = remote_.nodeId;
        // 自动填充发送方 Actor 名字（用于回程路由）
        packet.sourceActorName = system_ ? system_->GetActorName(actorId_) : "";
        packet.targetActorName = remote_.remoteName;
        packet.data = msg.data;
        packet.sessionId = msg.sessionId;
        packet.isResponse = msg.isResponse;
        // 通过传输层发送
        transport_->SendPacket(packet);
    }
};
```

#### 20.3.5 ClusterReceiver — 远程消息接收

```cpp
class ClusterReceiver {
public:
    ClusterReceiver(ActorSystem* sys);
    void OnPacketReceived(const ClusterPacket& packet);
};

// 实现（v2: 纯名字路由 + 回程信息填充）
void ClusterReceiver::OnPacketReceived(const ClusterPacket& packet) {
    ActorMessage msg{MsgType::UserMessage, 0, -1, packet.data};
    msg.sessionId = packet.sessionId;
    msg.isResponse = packet.isResponse;
    // 填充回程路由信息（接收方可用 RespondRemote() 自动回复）
    msg.sourceNodeId = packet.sourceNodeId;
    msg.sourceActorName = packet.sourceActorName;

    // 仅支持名字路由（actorId 跨进程无意义）
    if (!packet.targetActorName.empty()) {
        sys_->SendByName(packet.targetActorName, std::move(msg));
    }
}
```

> **关键变化（v2）**：
> - 移除了 `targetActorId` 路由路径，仅保留名字路由
> - `msg.sourceNodeId` 和 `msg.sourceActorName` 携带了发送方信息，接收方可用 `RespondRemote()` 自动回复

### 20.4 使用示例（同进程回环测试）

```cpp
// 创建两个 ActorSystem 模拟两个节点
ActorSystem nodeA_sys, nodeB_sys;

// 创建回环传输层
LoopbackTransport transportA, transportB;
transportA.SetLocalNodeId("nodeA");
transportB.SetLocalNodeId("nodeB");

// 创建接收器
ClusterReceiver receiverA(&nodeA_sys);
ClusterReceiver receiverB(&nodeB_sys);

// 交叉注册处理器
transportA.RegisterRemoteHandler("nodeB",
    [&](const ClusterPacket& pkt) { receiverB.OnPacketReceived(pkt); });
transportB.RegisterRemoteHandler("nodeA",
    [&](const ClusterPacket& pkt) { receiverA.OnPacketReceived(pkt); });

// 在 nodeA 创建代理，指向 nodeB 的 "echo" Actor
auto proxyId = nodeA_sys.RegisterActor(
    make_unique<ClusterProxy>(RemoteActorRef("nodeB", "echo"), &transportA));

// 发消息给代理 → 透明转发到 nodeB
nodeA_sys.Send(proxyId, ActorMessage{MsgType::UserMessage, 0, -1, "hello"});
```

### 20.5 TcpClusterTransport — 真实 TCP 跨网络传输

#### 20.5.1 设计动机

`LoopbackTransport` 仅用于单元测试，无法真正实现跨进程/跨机器通信。`TcpClusterTransport` 基于现有的 `EventLoop` + `Acceptor` + `Connector` 网络栈，实现了真实的 TCP 集群传输。

#### 20.5.2 架构总览

```
  ┌─────── Node A ────────────────┐     ┌─────── Node B ────────────────┐
  │                                │     │                                │
  │  SenderActor                   │     │  ReceiverActor (name="svc")   │
  │    │ SendToActor(proxyId)      │     │    ↑ OnMessage()              │
  │    ▼                           │     │    │                          │
  │  ClusterProxy("nodeB","svc")   │     │  ClusterGatewayActor (内部)    │
  │    │ OnMessage()               │     │    ↑ handleDecodedPacket()    │
  │    │ → transport.SendPacket()  │     │    │ → sys.SendByName("svc") │
  │    ▼                           │     │    │                          │
  │  TcpClusterTransport           │     │  TcpClusterTransport          │
  │    │ Encode() + conn.Send()    │     │    │ Acceptor 监听            │
  │    ▼                           │     │    ▼                          │
  │  Connector ──── TCP ────────── Acceptor   │                          │
  │                                │     │                                │
  └────────────────────────────────┘     └────────────────────────────────┘
```

#### 20.5.3 核心组件

| 组件 | 职责 |
|------|------|
| `ClusterPacketCodec` | 二进制序列化/反序列化。线协议：`[4:body_len][body]`，支持 TCP 流重组（部分包/粘包） |
| `ClusterGatewayActor` | 内部 Actor，处理集群 TCP I/O。per-fd 接收缓冲区实现流重组；处理握手包建立 fd↔nodeId 映射 |
| `TcpClusterTransport` | 实现 `IClusterTransport`。`Listen(port)` 创建 Acceptor，`ConnectToNode()` 创建 Connector，`SendPacket()` 序列化后通过 Connection 发送 |

#### 20.5.4 线协议 v2（Wire Protocol）

```
┌─────────────────────────────────────────────────────────────┐
│  [4 bytes: body_length, big-endian]                         │
├─────────────────────────────────────────────────────────────┤
│  [2 bytes: sourceNodeId.len][sourceNodeId bytes]  (Str16)   │
│  [2 bytes: targetNodeId.len][targetNodeId bytes]  (Str16)   │
│  [2 bytes: sourceActorName.len][sourceActorName]  (Str16)   │
│  [2 bytes: targetActorName.len][targetActorName]  (Str16)   │
│  [4 bytes: sessionId]                                       │
│  [1 byte:  flags (bit0=isResponse)]                         │
│  [4 bytes: data.len][data bytes]                  (Str32)   │
└─────────────────────────────────────────────────────────────┘
```

所有整数使用网络字节序（big-endian）。最大包大小限制 16MB。

> **v2 变化**：移除了 `sourceActorId` 和 `targetActorId`（跨进程无意义），新增 `sourceActorName` 用于回程路由。

#### 20.5.5 握手协议

连接建立后，双方各发送一个握手包：

```
握手包: {
    sourceNodeId = 本机 nodeId,
    targetNodeId = "",
    data = "CLUSTER_HANDSHAKE"
}
```

收到握手包后，提取 `sourceNodeId` 并建立 fd → nodeId 映射。

#### 20.5.6 TCP 流重组

`ClusterGatewayActor` 为每个 fd 维护接收缓冲区（`recvBuffers_[fd]`）：

```cpp
void ClusterGatewayActor::OnMessage(ActorMessage& msg) {
    if (msg.type == MsgType::NetworkRecv) {
        auto& buf = recvBuffers_[msg.fd];
        buf.append(msg.data);  // 追加新数据

        while (true) {
            ClusterPacket pkt;
            size_t consumed = ClusterPacketCodec::Decode(buf.data(), buf.size(), pkt);
            if (consumed == 0) break;  // 数据不完整
            handleDecodedPacket(pkt, msg.fd);
            buf.erase(0, consumed);
        }
    }
}
```

#### 20.5.7 使用示例（真实 TCP）

```cpp
// ---- 节点 B（服务端） ----
EventLoop loopB;  loopB.Create();
ActorSystem sysB; sysB.Start(4, &loopB); loopB.SetActorSystem(&sysB);

TcpClusterTransport transportB(&sysB, &loopB);
transportB.SetLocalNodeId("nodeB");
transportB.Listen(9600);  // 监听集群端口

auto echoId = sysB.RegisterActor(make_unique<EchoActor>());
sysB.RegisterName("echo_service", echoId);

// ---- 节点 A（客户端） ----
EventLoop loopA;  loopA.Create();
ActorSystem sysA; sysA.Start(4, &loopA); loopA.SetActorSystem(&sysA);

TcpClusterTransport transportA(&sysA, &loopA);
transportA.SetLocalNodeId("nodeA");
transportA.ConnectToNode("nodeB", "192.168.1.2", 9600);

// 创建代理 → 透明转发到节点 B
auto proxyId = sysA.RegisterActor(
    make_unique<ClusterProxy>(RemoteActorRef("nodeB", "echo_service"), &transportA));
sysA.Send(proxyId, ActorMessage{MsgType::UserMessage, 0, -1, "hello cluster!"});
```

#### 20.5.8 线程安全分析

| 数据结构 | 保护机制 | 说明 |
|----------|----------|------|
| `nodeToFd_` / `fdToNode_` | `SpinLock connLock_` | 多线程访问（worker 线程 SendPacket + IO 线程 RegisterNodeFd） |
| `pendingNodeId_` | `SpinLock pendingLock_` | ConnectToNode 预存 + 握手完成后清理 |
| `knownNodes_` | `SpinLock nodesLock_` | ConnectToNode 添加 + 握手/断连更新状态 |
| `recvBuffers_` | 无锁 | 仅在 ClusterGatewayActor::OnMessage 中串行访问 |
| `Connection::Send()` | `SpinLockQueue sendMQ_` | 内部已线程安全 |

### 20.6 跨进程便利 API — SendToRemote / RespondRemote

#### 20.6.1 ActorMessage 跨进程字段

```cpp
struct ActorMessage {
    // ... 原有字段 ...
    std::string sourceNodeId;      // 发送方节点 ID（跨进程回程路由）
    std::string sourceActorName;   // 发送方 Actor 名字（跨进程回程路由）

    bool IsRemote() const { return !sourceNodeId.empty(); }
};
```

当消息从远程节点到达时，`sourceNodeId` 和 `sourceActorName` 会被自动填充。接收方可通过 `msg.IsRemote()` 判断消息是否来自远程节点。

#### 20.6.2 Actor::SendToRemote — 直接发送跨进程消息

```cpp
// Actor.h
bool SendToRemote(const std::string& targetNodeId,
                  const std::string& targetActorName,
                  ActorMessage&& msg);
```

不需要创建 `ClusterProxy` 对象，直接通过 `ActorSystem` 注册的 transport 发送：

```cpp
class MyActor : public Actor {
    void someMethod() {
        ActorMessage msg{MsgType::UserMessage, 0, -1, "hello remote!"};
        SendToRemote("nodeB", "echo_service", std::move(msg));
    }
};
```

`SendToRemote` 内部会自动填充 `sourceActorName`（通过 `ActorSystem::GetActorName()` 反查）。

#### 20.6.3 Actor::RespondRemote — 自动回复远程请求

```cpp
// Actor.h
bool RespondRemote(const ActorMessage& request, const std::string& responseData);
```

接收到远程消息后，使用 `RespondRemote()` 可自动将回复路由回发送方：

```cpp
class EchoServiceActor : public Actor {
    void OnMessage(ActorMessage& msg) override {
        if (msg.IsRemote()) {
            // 自动读取 msg.sourceNodeId + msg.sourceActorName 进行回程路由
            RespondRemote(msg, "echo:" + msg.data);
        }
    }
};
```

等价于手动构造：
```cpp
ActorMessage resp{MsgType::UserMessage, actorId_, -1, "echo:" + msg.data};
resp.sessionId = msg.sessionId;
resp.isResponse = true;
system_->SendToRemote(msg.sourceNodeId, msg.sourceActorName, std::move(resp));
```

#### 20.6.4 ActorSystem::RegisterTransport — 注册传输层

```cpp
void ActorSystem::RegisterTransport(IClusterTransport* transport);
```

`SendToRemote()` 依赖已注册的 transport。启动时需调用：

```cpp
TcpClusterTransport transport(&sys, &loop);
transport.SetLocalNodeId("nodeA");
transport.ConnectToNode("nodeB", "192.168.1.2", 9600);
sys.RegisterTransport(&transport);  // 注册后 SendToRemote() 可用
```

### 20.7 后续扩展方向

- **序列化优化**：`ClusterPacket.data` 改用 protobuf / flatbuffers 序列化
- **节点发现**：通过心跳/注册中心自动发现节点
- **跨进程 Call/Response**：基于 `sessionId` 实现 `co_await ClusterCall()`
- **断线重连**：检测连接断开后自动重新建立
- **负载均衡**：同一 Actor 名字在多个节点上注册，代理端做轮询/随机路由

---

## 二十一、高级特性测试用例说明

### 21.1 测试文件

**文件：** `test_advanced_features.cc`
**构建：** `make -f Makefile.advanced`
**运行：** `./run_test_advanced.sh`

### 21.2 测试用例列表

| # | 测试名 | 验证内容 |
|---|--------|----------|
| 1 | TestPayloadTypeSystem | `std::any` payload 的 `SetPayload`/`GetPayload`/`IsPayloadType` 功能；验证多种类型（结构体、基本类型）的类型安全传递 |
| 2 | TestPriorityMessage | 双队列邮箱：先发 5 条普通消息再发 1 条优先级消息，验证优先级消息被先处理 |
| 3 | TestActorMetrics | `ActorMetrics` 计数器和计时器：验证 `totalMsgProcessed`、`totalProcessTimeUs`、`maxProcessTimeUs`、`avgProcessTimeUs` 的正确性；测试 `Reset()` 方法 |
| 4 | TestClusterProxy | `ClusterProxy` + `LoopbackTransport`：创建两个 ActorSystem 模拟两个节点，通过代理跨节点发送消息，验证名字寻址方式（v2 已移除 ID 寻址） |
| 5 | TestCollectActorStatsWithMetrics | `CollectActorStats()` 完整指标：注册 Actor、处理消息后，验证 `CollectActorStats()` 返回的指标数据（`totalMsgProcessed`、时间数据、`maxMailboxSize`）的正确性 |
| 6 | TestTcpCluster | `TcpClusterTransport` 真实 TCP 测试：两个独立的 EventLoop+ActorSystem（Node A / Node B）通过 TCP 连接，验证握手、正向消息投递（A→B）、反向确认（B→A）、ClusterPacketCodec 编解码一致性、粘包/半包处理 |

### 21.3 构建依赖

```makefile
# Makefile.advanced
SOURCES = test_advanced_features.cc \
          Actor.cc ActorSystem.cc EventLoop.cc Epollor.cc \
          ConnectionBase.cc Connection.cc Acceptor.cc Connector.cc \
          IOHelper.cc Timer.cc ClusterProxy.cc
```

### 21.4 测试架构说明

- Test1~5 为**纯 Actor 间消息通信**测试（无需网络），但因 `ActorSystem::Start()` 需要 `EventLoop*` 参数（用于 Timer 集成），测试中会创建 `EventLoop` 实例。
- Test6 (`TestTcpCluster`) 使用**真实 TCP 网络通信**，创建两套 EventLoop+ActorSystem 在 localhost 上通过 TCP 连接，验证完整的跨节点数据流。

每个测试函数独立创建 `ActorSystem`，测试完成后调用 `Stop()` 清理，确保测试间互不干扰。
