#include <iostream>
#include "AppLauncher.h"

using namespace bllsll;
int main(int argc, char* argv[])
{
    AppLauncher* pLauncher = new AppLauncher();
    if (!pLauncher) {
        std::cout << "new AppLauncher failed" << std::endl;
        return 0;
    }
    bool res = pLauncher->StartApp(argc, argv);
    if (!res) {
        std::cout << "new AppLauncher failed." << std::endl;
        return 0;
    }
    //执行
    pLauncher->Run();
    //停止
    delete pLauncher;
    return 0;
}