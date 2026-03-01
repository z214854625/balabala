#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: tcp服务
*/

#include "precompiled.h"
#include "IConnection.h"
#include "EventLoop.h"

namespace bllsll {

class TcpServer
{
public:
    void Start(int port);

private:
    IConnection* conn_ = nullptr;  // 生命周期由EventLoop的mapConn_管理
    EventLoop loop_;
};

} // namespace bllsll