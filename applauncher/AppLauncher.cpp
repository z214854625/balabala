#include <iostream>
#include <string>
#include <dlfcn.h>
#include "AppLauncher.h"

using namespace std;
using namespace bllsll;

AppLauncher::AppLauncher() 
    : _handle(nullptr), _app(nullptr), _stop(false)
{
}

AppLauncher::~AppLauncher()
{
    if (_app) {
        _app->Release();
        _app = nullptr;
    }
    // 关闭共享库
    dlclose(_handle);
}

bool AppLauncher::StartApp(int argc, char* argv[])
{
    //解析命令行参数
    ParseArgvs(argc, argv);
    string appName = FindArgv("appName");
    if (appName.empty()) {
        std::cout << "appName empty." << std::endl;
        return false;
    }
    string libName = "./lib" + appName + ".so";
    _handle = dlopen(libName, RTLD_LAZY | RTLD_GLOBAL);
    if (!_handle) {
        std::cout << "Failed to load library: " << dlerror() << std::endl;
        return false;
    }
    // 清除之前的错误
    dlerror();
    // 定义函数类型用于获取派生对象
    typedef IApplication* (*CreateAppFunc)();
    CreateAppFunc create = (CreateAppFunc)dlsym(_handle, "CreateAppFunction");
    if (!create) {
        std::cout << "Failed to find function: " << dlerror() << std::endl;
        dlclose(_handle);
        return false;
    }
    _app = create();
    if (!_app) {
        std::cout << "pApp null " << std::endl;
        dlclose(_handle);
        return false;
    }
    //启动程序
    if (!_app->OnStart()) {
        std::cout << "app start failed." << std::endl;
        dlclose(_handle);
        return false;
    }
    return true;
}

void AppLauncher::Run()
{
    while(true)
    {
        AppState state = _app->OnExec();
        if (state == AppState_Idle) {
            usleep(1600*1000);
        }
        else if(result == AppState_Finish) {
            break;
        }
    }
    // 释放资源
    if (_app) {
        _app->Release();
        _app = nullptr;
    }
}

void AppLauncher::ParseArgvs(int argc, char* argv[])
{
    _mapArgvs.clear();
    for (int i = 1; i < argc; ++i) {
        // 检查当前参数是否以 '-' 开头
        if (argv[i][0] == '-') {
            std::string key = argv[i] + 1;
            // 检查下一个参数是值而非选项
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string value = argv[i + 1];
                _mapArgvs.emplace(std::move(key), std::move(value));
                i++; // 跳过下一个参数
            }
        }
    }
    std::cout << "parse cmd line suc! count=" << _mapArgvs.size() << std::endl;
    for (const auto& pair : _mapArgvs) {
        std::cout << "parse cmd line. Key= " << pair.first << ", Value= " << pair.second << std::endl;
    }
}

string AppLauncher::FindArgv(string&& param)
{
    auto it = _mapArgvs.find(param);
    if (it != _mapArgvs.end()) {
        return it->second;
    }
    return "";
}
