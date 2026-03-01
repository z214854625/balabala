#include "precompiled.h"
#include "IConnection.h"
#include "TcpServer.h"
#include "TcpClient.h"

using namespace bllsll;
using namespace std;

int main(int argc, char *argv[])
{
    std::cout << "main start" << std::endl;
    int connect_flag = 0;
    int port = 0;
    std::string strIp = "";
    for (int i = 1; i < argc; ++i) {
        std::cout << "i= " << i << ", arg=" << argv[i] << std::endl;
        if (i == 1) {
            if ((std::strcmp(argv[i], "-s") == 0)) {
                connect_flag = 1;
            } else if (std::strcmp(argv[i], "-c") == 0) {
                connect_flag = 2;
            }else{
                std::cerr << "Unknown option: " << argv[i] << std::endl;
            }
        }else if(i == 2){
            port = atoi(argv[i]);
        }
        else if(i == 3){
            strIp = argv[i];
            // 去除可能由CRLF换行符引入的\r
            while (!strIp.empty() && (strIp.back() == '\r' || strIp.back() == '\n')) {
                strIp.pop_back();
            }
        }
    }
    std::cout << "net start! connect_flag= " << connect_flag << std::endl;
    try {
        bllsll::TcpServer tcpSvr;
        bllsll::TcpClient tcpCli;
        if (connect_flag == 1) {
            tcpSvr.Start(port);
        }else if(connect_flag == 2){
            tcpCli.Start(port, strIp);
        }
        getchar();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}