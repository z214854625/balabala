# Net 模块 Actor 模型改造设计文档

## 目录

- [一、原有代码在每个文件上的修改点及原因](#一原有代码在每个文件上的修改点及原因)
- [二、Actor 模型整体架构设计](#二actor-模型整体架构设计)
- [三、核心对象结构与职责](#三核心对象结构与职责)
- [四、数据流与代码逻辑详解](#四数据流与代码逻辑详解)
- [五、线程模型](#五线程模型)
- [六、扩展性说明](#六扩展性说明)

---

## 一、原有代码在每个文件上的修改点及原因

### 1. `IConnection.h` — 接口精简

**修改内容**：移除了 `RecvCallback`、`ConnCallback`、`DisConnCallback` 三个回调类型定义，以及 `OnRecv()`、`OnConnected()`、`OnDisconnected()` 三个虚函数。

**为什么要改**：回调模型下，业务逻辑通过 `std::function` 直接嵌入到连接对象中，导致连接层和业务层紧耦合。Actor 模型中，所有事件（数据到达、连接建立、断开）统一封装为 `ActorMessage` 投递给 Actor，连接对象只负责 I/O，不再持有业务回调。

```
改前: IConnection 有 9 个虚函数（含 OnRecv/OnConnected/OnDisconnected）
改后: IConnection 只有 5 个虚函数（Send/HandleRead/HandleWrite/HandleAccept/GetFd）
```

---

### 2. `ConnectionBase.h` — 移除回调成员

**修改内容**：移除 `recvCallback_`（RecvCallback）和 `disConnCallback_`（DisConnCallback）两个成员变量，以及对应的 `OnRecv()`、`OnDisconnected()` 方法。

**为什么要改**：原来 `HandleRead()` 读到数据后会把 `{fd, data, recvCallback_}` 三元组放入 `EventLoop::msgQueue_`，由主线程分发回调。Actor 模型中，数据直接通过 `ActorSystem::SendByFd()` 投递到绑定的 Actor，不需要存储回调。

---

### 3. `ConnectionBase.cc` — 核心路由逻辑变更

**修改内容**：

| 位置 | 改前 | 改后 |
|------|------|------|
| `HandleRead()` 收到数据 | `loop_->AddMsg({fd, str, recvCallback_})` | `actorSys->SendByFd(fd, ActorMessage{NetworkRecv, 0, fd, str})` |
| `HandleRead()` 检测断连 | 调用 `disConnCallback_(this)` | `actorSys->SendByFd(fd, ActorMessage{Disconnected, 0, fd, ""})` + `actorSys->UnbindFd(fd)` |
| 构造函数 | 初始化 `recvCallback_` | 无回调成员 |

**为什么要改**：这是整个 Actor 改造的核心。原来的 "I/O线程→消息队列→主线程回调" 三步变为 "I/O线程→Actor邮箱→工作线程处理" 三步。数据流的终点从回调函数变为 Actor 的 `OnMessage()` 方法。

`HandleWrite()` **未改动**，因为写逻辑（从 `sendMQ_` 取数据写 socket）与 Actor 模型无关，仍由 I/O 线程直接执行。

---

### 4. `Connection.h / Connection.cc` — 适配接口变更

**修改内容**：移除 `OnRecv()`、`OnConnected()`、`OnDisconnected()` 方法声明和实现。

**为什么要改**：`Connection` 继承自 `ConnectionBase`，父类已移除回调接口，子类同步移除。`Connection` 仅保留 I/O 相关的 `Send/HandleRead/HandleWrite/GetFd`。

---

### 5. `Acceptor.h` — 用 actorId 替代回调

**修改内容**：
- 构造函数参数从 `(int port, EventLoop* loop)` 改为 `(int port, EventLoop* loop, uint32_t listenerActorId)`
- 移除 `connCallback_` 成员（`ConnCallback` 类型）
- 新增 `listenerActorId_` 成员（`uint32_t` 类型）
- 移除 `OnRecv/OnConnected/OnDisconnected` 空实现

**为什么要改**：原来 Acceptor 通过 `connCallback_` 通知上层有新连接。Actor 模型中改为向 `listenerActorId_` 指定的 Actor 发送 `Connected` 消息。这样 Acceptor 不需要知道业务逻辑的任何细节。

---

### 6. `Acceptor.cc` — 新连接通过消息通知

**修改内容**：

```
改前 HandleAccept():
  1. accept() 得到 clientFd
  2. 创建 Connection, 加入 EventLoop
  3. connCallback_(pNewConn)     ← 调用回调
  4. 注册 epoll 事件

改后 HandleAccept():
  1. accept() 得到 clientFd
  2. 创建 Connection, 加入 EventLoop
  3. actorSys->BindFdToActor(clientFd, listenerActorId_)  ← 绑定fd到Actor
  4. actorSys->Send(listenerActorId_, {Connected, clientFd})  ← 发送消息
  5. 注册 epoll 事件
```

**为什么要改**：将 "回调通知" 改为 "消息通知"。`BindFdToActor` 建立了 fd 到 actorId 的映射，后续该 fd 上的数据会自动路由到同一个 Actor。

---

### 7. `Connector.h` — 用 actorId 替代回调

**修改内容**：
- 构造函数参数新增 `uint32_t ownerActorId`
- 移除 `connCallback_` 成员
- 新增 `ownerActorId_` 成员
- 构造函数中直接调用 `_Connect()`（原来在 `OnConnected()` 中触发）
- 移除 `OnRecv/OnConnected/OnDisconnected`

**为什么要改**：与 Acceptor 对称改造。客户端连接成功后向 `ownerActorId_` 发送 `Connected` 消息。连接在构造时直接发起，不再需要先设回调再连接的两步操作。

---

### 8. `Connector.cc` — 连接完成后发消息

**修改内容**：

```
改前 HandleWrite() (connecting_阶段):
  1. getsockopt() 检查连接结果
  2. connCallback_(this)                    ← 调用回调
  3. ModifyEvent(读模式)

改后 HandleWrite() (connecting_阶段):
  1. getsockopt() 检查连接结果
  2. actorSys->BindFdToActor(fd, ownerActorId_)  ← 绑定fd
  3. actorSys->Send(ownerActorId_, {Connected, fd})  ← 发送消息
  4. ModifyEvent(读模式)
```

**为什么要改**：非阻塞 connect 完成后的通知方式从回调改为 Actor 消息。

---

### 9. `EventLoop.h` — 移除消息队列分发

**修改内容**：
- 移除 `using recvMsgType = std::tuple<int, std::string, RecvCallback>`
- 移除 `SpinLockQueue<recvMsgType> msgQueue_`
- 移除 `void OnDispatch(int timeout)` 方法
- 移除 `void AddMsg(recvMsgType&&)` 方法
- 移除 `using Functor = std::function<void()>`
- 新增 `ActorSystem* actorSystem_` 指针
- 新增 `SetActorSystem()` / `GetActorSystem()` 方法
- 移除对 `SpinLockQueue.h` 的引用（不再需要消息队列）

**为什么要改**：原来 `EventLoop` 承担了两个职责：(1) epoll I/O 循环 (2) 消息分发给主线程。Actor 模型中，职责 (2) 由 `ActorSystem` 接管。`EventLoop` 只需持有 `ActorSystem*` 指针供 I/O 组件访问。

---

### 10. `EventLoop.cc` — 移除分发代码

**修改内容**：移除 `OnDispatch()` 和 `AddMsg()` 方法实现。其余代码（`run()`、`AddEvent()`、连接管理）保持不变。

**为什么要改**：`OnDispatch()` 是主线程轮询消息队列的方法，Actor 模型中工作线程主动从 Actor 邮箱取消息，不再需要主线程轮询。

---

### 11. `TcpServer.h / TcpServer.cc` — 使用 EchoServerActor

**修改内容**：
- 新增 `EchoServerActor` 类（继承 `Actor`），实现 `OnMessage()` 处理三种消息
- `TcpServer` 成员从 `IConnection* conn_` 改为 `Acceptor* acceptor_` + `ActorSystem actorSystem_` + `uint32_t serverActorId_`
- `Start()` 流程从 "设回调→轮询OnDispatch" 改为 "注册Actor→启动Acceptor→主线程sleep"

**为什么要改**：
```
改前 Start():
  loop_.Create()
  conn_ = new Acceptor(port, &loop_)
  conn_->OnConnected([&](IConnection* p) {
      p->OnRecv([](conn, data, len) { conn->Send(data, len); })  // echo
      p->OnDisconnected(...)
  })
  while(1) { loop_.OnDispatch(500); usleep(25000); }  // 主线程轮询

改后 Start():
  loop_.Create()
  actorSystem_.Start(4, &loop_)
  loop_.SetActorSystem(&actorSystem_)
  serverActorId_ = actorSystem_.RegisterActor(make_unique<EchoServerActor>())
  acceptor_ = new Acceptor(port, &loop_, serverActorId_)
  while(1) { usleep(100000); }  // 主线程仅保活，业务在工作线程处理
```

业务逻辑从嵌套lambda回调 → 集中在 `EchoServerActor::OnMessage()` 的 switch-case 中。

---

### 12. `TcpClient.h / TcpClient.cc` — 使用 EchoClientActor

**修改内容**：与 TcpServer 对称改造。
- 新增 `EchoClientActor` 类
- `Start()` 创建 ActorSystem、注册 Actor、创建 Connector
- 移除分发线程（原来有一个专门的 `std::thread` 调 `OnDispatch`）

**为什么要改**：原来需要一个额外线程做 `OnDispatch()`，Actor 模型自带工作线程池，不再需要手动分发。

---

### 13. `Util/SpinLockQueue.h` — 性能优化

**修改内容**：
- 新增 `void push(T&& value)` 右值引用重载
- `pop()` 中 `T value = queue.front()` 改为 `T value = std::move(queue.front())`

**为什么要改**：`ActorMessage` 包含 `std::string data` 成员，频繁的 push/pop 会触发字符串拷贝。添加 move 语义后，消息传递全程零拷贝（仅移动指针）。

---

## 二、Actor 模型整体架构设计

### 改造前后架构对比

```
╔══════════════════════════════════════════════════════════════════╗
║                        改 造 前（回调模型）                       ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║   ┌──────────┐    ┌─────────────────────────────────┐            ║
║   │  Client  │    │         EventLoop                │            ║
║   │  Socket  │───▷│  epoll loop 线程                 │            ║
║   └──────────┘    │  ┌─────────────┐                │            ║
║                   │  │ HandleRead()│                │            ║
║                   │  │  读取数据    │                │            ║
║                   │  └──────┬──────┘                │            ║
║                   │         │                        │            ║
║                   │         ▼                        │            ║
║                   │  ┌──────────────┐               │            ║
║                   │  │  msgQueue_   │ SpinLockQueue  │            ║
║                   │  │ {fd,data,cb} │               │            ║
║                   │  └──────┬───────┘               │            ║
║                   └─────────┼───────────────────────┘            ║
║                             │                                    ║
║                             ▼                                    ║
║                   ┌─────────────────┐                            ║
║                   │    主线程        │                            ║
║                   │  OnDispatch()   │  轮询消息队列               ║
║                   │  callback(conn, │  调用业务回调               ║
║                   │    data, len)   │                            ║
║                   └─────────────────┘                            ║
║                                                                  ║
║  问题：                                                          ║
║  1. 业务回调（lambda）散落在 TcpServer::Start() 的嵌套闭包中       ║
║  2. 主线程必须轮询 OnDispatch()，浪费 CPU                         ║
║  3. 回调中的 this 指针生命周期管理复杂                             ║
║  4. 难以扩展为多 Actor 并行处理                                   ║
╚══════════════════════════════════════════════════════════════════╝


╔══════════════════════════════════════════════════════════════════╗
║                       改 造 后（Actor 模型）                      ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║   ┌──────────┐    ┌─────────────────────────────────┐            ║
║   │  Client  │    │         EventLoop                │            ║
║   │  Socket  │───▷│  epoll loop 线程                 │            ║
║   └──────────┘    │  ┌─────────────┐                │            ║
║                   │  │ HandleRead()│                │            ║
║                   │  │  读取数据    │                │            ║
║                   │  └──────┬──────┘                │            ║
║                   │         │                        │            ║
║                   └─────────┼───────────────────────┘            ║
║                             │                                    ║
║                             ▼                                    ║
║                   ┌─────────────────────────────────┐            ║
║                   │        ActorSystem               │            ║
║                   │  ┌───────────────┐              │            ║
║                   │  │ fdToActor_    │  fd→actorId   │            ║
║                   │  │ {fd: actorId} │  映射表       │            ║
║                   │  └───────┬───────┘              │            ║
║                   │          │ SendByFd()            │            ║
║                   │          ▼                        │            ║
║                   │  ┌───────────────┐              │            ║
║                   │  │ Actor.mailbox_│ 邮箱          │            ║
║                   │  │ [msg1][msg2]..│               │            ║
║                   │  └───────┬───────┘              │            ║
║                   │          │                        │            ║
║                   │          ▼                        │            ║
║                   │  ┌───────────────┐              │            ║
║                   │  │ readyQueue_   │ 就绪队列      │            ║
║                   │  │ [actorId...]  │               │            ║
║                   │  └───────┬───────┘              │            ║
║                   └──────────┼──────────────────────┘            ║
║                              │ cv_.notify_one()                  ║
║              ┌───────────────┼───────────────┐                   ║
║              ▼               ▼               ▼                   ║
║   ┌──────────────┐ ┌──────────────┐ ┌──────────────┐            ║
║   │  Worker #1   │ │  Worker #2   │ │  Worker #N   │            ║
║   │ ProcessOne() │ │ ProcessOne() │ │ ProcessOne() │            ║
║   │ OnMessage()  │ │ OnMessage()  │ │ OnMessage()  │            ║
║   └──────────────┘ └──────────────┘ └──────────────┘            ║
║                                                                  ║
║  优势：                                                          ║
║  1. 业务逻辑集中在 Actor::OnMessage() 的 switch-case 中          ║
║  2. 工作线程自动调度，无需主线程轮询                               ║
║  3. Actor 邮箱保证消息串行处理，天然线程安全                       ║
║  4. 可扩展为多 Actor 并行（每连接一个 Actor）                      ║
╚══════════════════════════════════════════════════════════════════╝
```

---

## 三、核心对象结构与职责

### 类继承关系图

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
    EchoServerActor   EchoClientActor
     (服务端业务)      (客户端业务)
```

### 对象职责表

| 对象 | 层次 | 职责 |
|------|------|------|
| `EventLoop` | I/O层 | 管理 epoll loop 线程，维护 fd↔IConnection 映射，持有 `ActorSystem*` |
| `Epollor` | I/O层 | epoll 系统调用封装（`epoll_create/ctl/wait`） |
| `IConnection` | I/O层 | 连接接口（`Send/HandleRead/HandleWrite/HandleAccept/GetFd`） |
| `ConnectionBase` | I/O层 | 读写逻辑实现，读到数据→投递 `ActorMessage`，写数据从 `sendMQ_` 取 |
| `Connection` | I/O层 | 服务端 accept 到的客户端连接 |
| `Connector` | I/O层 | 客户端主动连接（非阻塞 connect + EPOLLOUT 确认） |
| `Acceptor` | I/O层 | 监听端口，accept 新连接，绑定 fd→Actor，发 `Connected` 消息 |
| `ActorMessage` | 消息层 | 消息结构体（`type`, `sourceId`, `fd`, `data`） |
| `Actor` | 业务层 | 基类，含邮箱（`mailbox_`）、调度标记（`scheduled_`），派生类实现 `OnMessage()` |
| `ActorSystem` | 调度层 | Actor 注册/注销、fd↔actorId 映射、工作线程池、就绪队列调度 |
| `EchoServerActor` | 业务层 | 服务端业务（echo 回传） |
| `EchoClientActor` | 业务层 | 客户端业务（打印收到的数据） |
| `TcpServer` | 入口层 | 组装 EventLoop + ActorSystem + Acceptor + EchoServerActor |
| `TcpClient` | 入口层 | 组装 EventLoop + ActorSystem + Connector + EchoClientActor |

---

## 四、数据流与代码逻辑详解

### 场景1：服务端收到客户端数据（Echo 流程）

```
  客户端发送 "hello"
       │
       ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │ epoll loop 线程                                                 │
  │                                                                 │
  │  epoll_wait() 返回 EPOLLIN 事件 (clientFd=5)                    │
  │       │                                                         │
  │       ▼                                                         │
  │  callbacks_[5] → lambda → pConn->HandleRead(5, event)           │
  │       │                                                         │
  │       ▼                                                         │
  │  ConnectionBase::HandleRead(fd=5)                               │
  │    ├─ recv(5, buffer, 64KB) → n=5, buffer="hello"               │
  │    ├─ 构造 ActorMessage{NetworkRecv, 0, 5, "hello"}             │
  │    └─ loop_->GetActorSystem()->SendByFd(5, msg)                 │
  │            │                                                    │
  └────────────┼────────────────────────────────────────────────────┘
               │
               ▼
  ┌──────────────────────────────────────────────────────┐
  │ ActorSystem::SendByFd(fd=5, msg)                     │
  │                                                      │
  │  1. fdToActor_[5] = 1  → actorId = 1                │
  │  2. Send(actorId=1, msg)                             │
  │     ├─ actors_[1] → EchoServerActor                  │
  │     ├─ actor->PushMessage(msg)  → 放入 mailbox_      │
  │     └─ scheduled_ CAS false→true                     │
  │        ├─ readyQueue_.push(1)                        │
  │        └─ cv_.notify_one()                           │
  └──────────────────────────────────────────────────────┘
               │
               ▼
  ┌──────────────────────────────────────────────────────┐
  │ Worker 线程 #2 (被 cv_ 唤醒)                         │
  │                                                      │
  │  workerLoop():                                       │
  │    1. readyQueue_.pop() → actorId = 1                │
  │    2. actors_[1] → EchoServerActor*                  │
  │    3. actor->ProcessOne()                            │
  │       ├─ mailbox_.pop() → ActorMessage               │
  │       └─ OnMessage(msg)                              │
  │            │                                         │
  │            ▼                                         │
  │  EchoServerActor::OnMessage()                        │
  │    case NetworkRecv:                                 │
  │      SendToNetwork(fd=5, "hello", 5)                 │
  │        │                                             │
  │        ▼                                             │
  │  Actor::SendToNetwork()                              │
  │    └─ loop->GetConnection(5)->Send("hello", 5)       │
  │         │                                            │
  │         ▼                                            │
  │  ConnectionBase::Send("hello", 5)                    │
  │    ├─ sendMQ_.push("hello")                          │
  │    └─ poller->ModifyEvent(5, EPOLL_EVENTS_RW)        │
  └──────────────────────────────────────────────────────┘
               │
               ▼
  ┌──────────────────────────────────────────────────────┐
  │ epoll loop 线程 (下一轮 epoll_wait)                   │
  │                                                      │
  │  EPOLLOUT 事件触发 (fd=5)                             │
  │       │                                              │
  │       ▼                                              │
  │  ConnectionBase::HandleWrite(fd=5)                   │
  │    ├─ sendMQ_.pop() → "hello"                        │
  │    ├─ write(5, "hello", 5) → 发送到客户端              │
  │    └─ ModifyEvent(5, EPOLL_EVENTS_R)                 │
  └──────────────────────────────────────────────────────┘
```

### 场景2：服务端 Accept 新连接

```
  客户端发起 connect()
       │
       ▼
  ┌─────────────────────────────────────────────────────┐
  │ epoll loop 线程                                      │
  │                                                      │
  │  epoll_wait() 返回 EPOLLIN (listenFd=3)              │
  │       │                                              │
  │       ▼                                              │
  │  Acceptor::HandleAccept(listenFd=3)                  │
  │    ├─ accept() → clientFd = 5                        │
  │    ├─ SetNonBlocking(5)                              │
  │    ├─ new Connection(5, loop_)                       │
  │    ├─ loop_->AddConnection(pNewConn)                 │
  │    ├─ actorSys->BindFdToActor(5, listenerActorId_=1) │  ← 新增
  │    ├─ actorSys->Send(1, {Connected, 0, 5, ""})       │  ← 新增
  │    └─ loop_->AddEvent(5, EPOLL_EVENTS_RW, lambda)    │
  └──────────────────────────────────────────────────────┘
               │
               ▼
  ┌──────────────────────────────────────────────────────┐
  │ Worker 线程                                          │
  │                                                      │
  │  EchoServerActor::OnMessage()                        │
  │    case Connected:                                   │
  │      cout << "new client connected! fd=5"            │
  └──────────────────────────────────────────────────────┘
```

### 场景3：客户端连接服务器

```
  ┌─────────────────────────────────────────────────────┐
  │ 主线程                                               │
  │                                                      │
  │  TcpClient::Start(9527, "127.0.0.1")                │
  │    ├─ loop_.Create()          → 启动 epoll loop 线程  │
  │    ├─ actorSystem_.Start(2)   → 启动 2 个工作线程     │
  │    ├─ RegisterActor(EchoClientActor) → actorId=1     │
  │    └─ new Connector(&loop_, 9527, "127.0.0.1", 1)   │
  │         │                                            │
  │         ▼                                            │
  │  Connector::_Connect()                               │
  │    ├─ socket() → fd=4                                │
  │    ├─ fcntl(O_NONBLOCK)                              │
  │    ├─ connect() → EINPROGRESS (非阻塞连接中)          │
  │    ├─ connecting_ = true                             │
  │    ├─ loop_->AddEvent(4, EPOLL_EVENTS_RW, lambda)    │
  │    └─ loop_->AddConnection(this)                     │
  └──────────────────────────────────────────────────────┘
               │
               ▼  (连接完成后 EPOLLOUT 触发)
  ┌──────────────────────────────────────────────────────┐
  │ epoll loop 线程                                      │
  │                                                      │
  │  Connector::HandleWrite(fd=4)                        │
  │    ├─ connecting_ == true                            │
  │    ├─ getsockopt(SO_ERROR) → err=0 (连接成功)         │
  │    ├─ connecting_ = false                            │
  │    ├─ actorSys->BindFdToActor(4, ownerActorId_=1)    │
  │    ├─ actorSys->Send(1, {Connected, 0, 4, ""})       │
  │    └─ ModifyEvent(4, EPOLL_EVENTS_R)                 │
  └──────────────────────────────────────────────────────┘
               │
               ▼
  ┌──────────────────────────────────────────────────────┐
  │ Worker 线程                                          │
  │                                                      │
  │  EchoClientActor::OnMessage()                        │
  │    case Connected:                                   │
  │      cout << "connected to server! fd=4"             │
  └──────────────────────────────────────────────────────┘
```

---

## 五、线程模型

```
┌─────────────────────────────────────────────────────────────────┐
│                         线程模型总览                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌────────────────┐                                             │
│  │   主线程        │  TcpServer: sleep / TcpClient: stdin 读取   │
│  │                │  可调用 conn->Send() (线程安全)               │
│  └────────────────┘                                             │
│                                                                  │
│  ┌────────────────┐                                             │
│  │ epoll loop线程  │  唯一的 I/O 线程                             │
│  │                │  负责: epoll_wait → HandleRead/Write/Accept  │
│  │                │  保证: 同一 fd 的 I/O 串行执行                 │
│  │                │  产出: ActorMessage 投递到 Actor 邮箱          │
│  └────────────────┘                                             │
│                                                                  │
│  ┌────────────────┐ ┌────────────────┐ ┌────────────────┐       │
│  │  Worker #1     │ │  Worker #2     │ │  Worker #N     │       │
│  │                │ │                │ │                │       │
│  │ 从 readyQueue  │ │ 从 readyQueue  │ │ 从 readyQueue  │       │
│  │ 取 actorId     │ │ 取 actorId     │ │ 取 actorId     │       │
│  │ 调 ProcessOne()│ │ 调 ProcessOne()│ │ 调 ProcessOne()│       │
│  │ 执行 OnMessage │ │ 执行 OnMessage │ │ 执行 OnMessage │       │
│  └────────────────┘ └────────────────┘ └────────────────┘       │
│                                                                  │
│  线程安全保证:                                                    │
│  ─────────────                                                   │
│  • Actor.mailbox_ (SpinLockQueue) — 线程安全队列                  │
│  • Actor.scheduled_ (atomic<bool>) — CAS 无锁调度标记             │
│  • ActorSystem.actors_ (SpinLock) — 保护 Actor 注册表             │
│  • ActorSystem.fdToActor_ (SpinLock) — 保护 fd 映射表             │
│  • ActorSystem.readyQueue_ (SpinLockQueue) — 线程安全就绪队列     │
│  • EventLoop.callbacks_ (SpinLock) — 保护 epoll 回调映射          │
│  • EventLoop.mapConn_ (SpinLock) — 保护连接对象映射               │
│  • ConnectionBase.sendMQ_ (SpinLockQueue) — 线程安全发送队列      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Actor 调度算法

```
Send(actorId, msg):
    actor->PushMessage(msg)                    // 放入邮箱
    if CAS(actor->scheduled_, false → true):   // 原子标记
        readyQueue_.push(actorId)              // 加入就绪队列
        cv_.notify_one()                       // 唤醒一个 worker

workerLoop():
    while running:
        actorId = readyQueue_.pop()            // 取出就绪 actor
        if !actorId: cv_.wait(10ms); continue  // 无任务则等待

        actor = actors_[actorId]
        processed = 0
        while actor->ProcessOne() && processed < 64:  // 批量处理，最多64条
            processed++

        if !actor->mailbox_.empty():           // 还有消息
            readyQueue_.push(actorId)          // 重新调度
        else:
            actor->scheduled_ = false          // 取消调度
            // 双重检查（防止在取消和检查之间有新消息）
            if !actor->mailbox_.empty():
                if CAS(scheduled_, false→true):
                    readyQueue_.push(actorId)
```

关键设计点：
- **scheduled_ 标记**：避免同一个 Actor 被多个 Worker 同时处理，保证 Actor 内消息串行
- **批量处理 64 条**：避免一个 Actor 消息过多时饿死其他 Actor
- **双重检查**：解决 "取消标记" 和 "检查邮箱" 之间的竞态条件

---

## 六、扩展性说明

### 自定义 Actor 示例

```cpp
// 每个连接一个独立 Actor 的服务器
class PerConnServerActor : public Actor {
public:
    void OnMessage(ActorMessage& msg) override {
        switch (msg.type) {
        case MsgType::Connected: {
            // 为新连接创建独立的 Actor
            auto connActor = std::make_unique<MyConnectionActor>();
            uint32_t newId = system_->RegisterActor(std::move(connActor));
            // 重新绑定 fd 到新 Actor（后续数据直接路由到它）
            system_->BindFdToActor(msg.fd, newId);
            break;
        }
        case MsgType::Disconnected:
            // 注销对应的 Actor
            // system_->UnregisterActor(...)
            break;
        default: break;
        }
    }
};
```

### Actor 间通信

```cpp
class GameActor : public Actor {
    void OnMessage(ActorMessage& msg) override {
        if (msg.type == MsgType::UserMessage) {
            // 处理来自其他 Actor 的消息
        }
        // 发送消息给其他 Actor
        SendToActor(otherActorId, ActorMessage{
            MsgType::UserMessage, 0, -1, "some data"
        });
    }
};
```
