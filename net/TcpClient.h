#pragma once
/**
@auther: chencaiyu
@date: 2024.8.11
@brief: tcp客户端
*/

#include "precompiled.h"
#include "IConnection.h"
#include "EventLoop.h"

namespace sll {

class TcpClient
{
public:
    void Start(int port, const std::string strIp);

private:
    std::unique_ptr<IConnection> conn_;
    EventLoop loop_;
};

} // namespace sll