#include "TcpServer.h"
#include "Connection.h"

using namespace sll;

void TcpServer::Start()
{
    int port = 9527;
    loop_.Create();
    conn_ = std::make_unique<Connection>(port, &loop_);
    conn_->OnRecv([this](const char* pData, int nLen) {
        //recv msg
        std::cout << pData << std::endl;
        //conn_->Send(string(pData, nLen));
    });
}