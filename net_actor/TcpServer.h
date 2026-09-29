#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: tcp服务（Actor模型版本）
*/

#include "precompiled.h"
#include "EventLoop.h"
#include "EventLoopThreadPool.h"
#include "ActorSystem.h"
#include "Actor.h"

namespace bllsll {

class Acceptor;

// Echo服务Actor：处理所有客户端的连接/断连/数据事件
class EchoServerActor : public Actor
{
public:
    ActorTask OnCoroutineMessage(ActorMessage msg) override;
};

class TcpServer
{
public:
    // [2026.5 多 Reactor] 启动 TcpServer。
    //   subReactorNum=0（默认）：单 Reactor 模式，完全兼容旧调用 Start(port)
    //   subReactorNum>0       ：主从 Reactor 模式，主 loop 只跑 Acceptor，
    //                            新连接 Round-Robin 派发到 N 个 SubReactor
    //   workerCount           ：ActorSystem 业务 worker 线程数（默认 4）
    void Start(int port, int subReactorNum = 0, int workerCount = 4);
    // [P1-8 修复] 优雅关停：先停 I/O（loop/sub pool）→ 再停 ActorSystem → 释放 acceptor_
    void Stop();

private:
    // [P1-8 修复] 成员声明顺序：actorSystem_ 先声明 → 后析构（保证 I/O 先停，ActorSystem 后停）
    // 原顺序 loop_ 在前、actorSystem_ 在后，导致 actorSystem_ 先析构时 loop_ 仍在跑 → UAF
    ActorSystem actorSystem_;
    EventLoop loop_;                          // 主 loop（跑 Acceptor）
    EventLoopThreadPool subReactorPool_;      // [2026.5] SubReactor 池（subReactorNum>0 时启用）
    std::unique_ptr<Acceptor> acceptor_;     // [P0-2 修复] 改为 unique_ptr 管理
    uint32_t serverActorId_ = 0;
    std::atomic<bool> running_{false};       // [P1-8] 主循环保活标志
};

} // namespace bllsll
