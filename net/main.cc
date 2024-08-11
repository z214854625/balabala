#include "precompiled.h"
#include "IConnection.h"
#include "TcpServer.h"
#include "TcpClient.h"

using namespace sll;
using namespace std;

int main(int argc, char *argv[])
{
    std::cout << "main start" << std::endl;
    // int connect_flag = 0;
    // for (int i = 1; i < argc; ++i) {
    //     if ((std::strcmp(argv[i], "-s") == 0)) {
    //         connect_flag = 1;
    //     } else if (std::strcmp(argv[i], "-c") == 0) {
    //         connect_flag = 2;
    //     }else{
    //         std::cerr << "Unknown option: " << argv[i] << std::endl;
    //     }
    //     break;
    // }
    // std::cout << "net start! connect_flag= " << connect_flag << std::endl;
    // if (connect_flag == 1) {
    //     sll::TcpServer tcpSvr;
    //     tcpSvr.Start();
    // }else if(connect_flag == 2){
    // sll::TcpClient tcpCli;
    //     tcpCli.Start();
    // }
    getchar();

    return 0;
}