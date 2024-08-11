#include "TcpServer.h"
#include "Connection.h"

using namespace sll;
using namespace std;

void TcpServer::Start()
{
    int port = 9527;
    loop_.Create(8);
    //conn_ = std::make_unique<Connection>(port, &loop_);
    conn_.reset(new Connection(port, &loop_));
    conn_->OnRecv([this](const char* pData, int nLen) {
        std::cout << "server recv msg: " << pData << std::endl;
        conn_->Send(pData, nLen);
    });
    std::cout << "TcpServer::Start suc!" << std::endl;
}