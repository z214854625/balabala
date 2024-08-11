#include "IConnection.h"
#include "TcpServer.h"

using namespace sll;
using namespace std;

int main()
{
    sll::TcpServer tcpSvr;
    tcpSvr.Start();
    
    getchar();

    return 0;
}