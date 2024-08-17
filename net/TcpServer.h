#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: tcp服务
*/

#include "precompiled.h"
#include "IConnection.h"
#include "EventLoop.h"

namespace sll {

class TcpServer
{
public:
    void Start(int port);

private:
    std::unique_ptr<IConnection> conn_;
    EventLoop loop_;
};

} // namespace sll