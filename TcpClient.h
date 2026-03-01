#pragma once
/**
@auther: chencaiyu
@date: 2024.8.11
@brief: tcp客户端
*/

#include "precompiled.h"
#include "IConnection.h"
#include "EventLoop.h"

namespace bllsll {

class TcpClient
{
public:
    void Start(int port, const std::string strIp);

private:
    IConnection* conn_ = nullptr;  // 生命周期由EventLoop的mapConn_管理
    EventLoop loop_;
};

} // namespace bllsll