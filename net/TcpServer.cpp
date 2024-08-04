#include "TcpServer.h"
#include "Connection.h"

using namespace sll;

void TcpServer::Start()
{
    int port = 9527;
    loop_.Create();
    conn_ = std::make_unique<Connection>(port, &loop_);
}