#pragma once
/**
@auther: chencaiyu
@date: 2024.8.11
@brief: tcp客户端（Actor模型版本）
*/

#include "precompiled.h"
#include "EventLoop.h"
#include "ActorSystem.h"
#include "Actor.h"

namespace bllsll {

class Connector;

// 客户端Actor：处理与服务器的通信事件
class EchoClientActor : public Actor
{
public:
    void OnMessage(ActorMessage& msg) override;
};

class TcpClient
{
public:
    void Start(int port, const std::string strIp);

private:
    EventLoop loop_;
    ActorSystem actorSystem_;
    IConnection* conn_ = nullptr;  // 生命周期由EventLoop的mapConn_管理
    uint32_t clientActorId_ = 0;
};

} // namespace bllsll
