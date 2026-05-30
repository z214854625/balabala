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

private:
    EventLoop loop_;                          // 主 loop（跑 Acceptor）
    EventLoopThreadPool subReactorPool_;      // [2026.5] SubReactor 池（subReactorNum>0 时启用）
    ActorSystem actorSystem_;
    Acceptor* acceptor_ = nullptr;
    uint32_t serverActorId_ = 0;
};

} // namespace bllsll
