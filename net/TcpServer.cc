#include "TcpServer.h"
#include "Acceptor.h"

using namespace bllsll;
using namespace std;

void TcpServer::Start(int port)
{
    std::cout << "TcpServer::Start 1--" << std::endl;
    //启动eventloop（仅一个epoll loop线程，所有I/O在该线程中串行执行）
    loop_.Create();
    //启动tcp服务器（Acceptor由EventLoop的mapConn_管理生命周期）
    conn_ = new Acceptor(port, &loop_);
    //收到连接
    conn_->OnConnected([&](IConnection* pCliConn) {
        std::cout << "new client connection! fd=" << pCliConn->GetFd() << std::endl;
        //接收数据
        pCliConn->OnRecv([&](IConnection* pConn, const char* pData, int nLen) {
            std::cout << "svr recv msg! nLen="<< nLen << ", data=" << "" << ", fd= " << pConn->GetFd() << std::endl;
            pConn->Send(pData, nLen);
        });
        //断开连接
        pCliConn->OnDisconnected([&](IConnection* pConn) {
            std::cout << "client disconnected! fd=" << pConn->GetFd() << std::endl;
        });
    });
    while (1) {
        loop_.OnDispatch(500);
        usleep(25000);
    }
}