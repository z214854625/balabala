#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: tcp服务（Actor模型版本）
*/

#include "precompiled.h"
#include "EventLoop.h"
#include "ActorSystem.h"
#include "Actor.h"

namespace bllsll {

class Acceptor;

// Echo服务Actor：处理所有客户端的连接/断连/数据事件
class EchoServerActor : public Actor
{
public:
    void OnMessage(ActorMessage& msg) override;
};

class TcpServer
{
public:
    void Start(int port);

private:
    EventLoop loop_;
    ActorSystem actorSystem_;
    Acceptor* acceptor_ = nullptr;  // 生命周期由EventLoop的mapConn_管理
    uint32_t serverActorId_ = 0;
};

} // namespace bllsll
