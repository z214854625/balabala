#pragma once
/**
@auther: chencaiyu
@date: 2025.1.5
@brief: 程序启动器
*/

namespace bllsll {

enum AppState
{
    AppState_Unknown = 0,     //未知
    AppState_Process = 1,     //处理状态
    AppState_Idle = 2,        //空闲状态
    AppState_Finish = 3,      //完成状态
};

class IAppLauncher
{
public:
    virtual ~IAppLauncher() {}
};

class IApplication
{
public:
    virtual ~IApplication() {}
    //初始化
    virtual bool OnStart(IAppLauncher* pLauncher) = 0;
    //释放
    virtual void Release() = 0;
    //执行业务
    virtual AppState OnExec() = 0;
};

} // namespace bllsll
