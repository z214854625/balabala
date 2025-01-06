#pragma once
/**
@auther: chencaiyu
@date: 2024.1.5
@brief: 程序启动器
*/
#include <map>
#include "../include/IApplication.h"

namespace bllsll {

class AppLauncher : public IAppLauncher
{
public:
    AppLauncher();
    virtual ~AppLauncher();
    //启动程序
    bool StartApp(int argc, char* argv[]);
    //执行函数
    void Run();
    //解析命令行参数
    void ParseArgvs(int argc, char* argv[]);
    //查找命令
    std::string FindArgv(string&& param);

private:
    std::map<string, string> _mapArgvs; //命令行参数
    void* _handle;
    IApplication* _app;
    bool _stop;
};

} // namespace bllsll