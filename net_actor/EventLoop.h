#pragma once
/**
@auther: chencaiyu
@date: 2024.8.1
@brief: I/O事件循环处理（Actor模型版本）
线程模型：
- 1个 epoll loop 线程：负责所有I/O操作（accept/read/write），保证同一fd的I/O串行
- Actor工作线程池：负责业务逻辑处理（由ActorSystem管理）
- I/O线程读到数据后，通过ActorSystem路由给对应的Actor
- Send() 可从任意线程调用，通过 SpinLockQueue + epoll_ctl 保证线程安全
*/

#include "precompiled.h"
#include "../Util/SpinLock.h"
#include "IConnection.h"

namespace bllsll {

class Poller;
class ActorSystem;

class EventLoop
{
public:
    using Callback = std::function<void(int, uint32_t)>;

    EventLoop();
    ~EventLoop();
    //创建（启动epoll loop线程）
    void Create();
    //事件循环（在loop线程中直接执行I/O回调）
    void run(int timeout = -1);
    //添加io事件
    void AddEvent(int fd, uint32_t events, Callback&& cb);
    //修改io事件
    void ModifyEvent(int fd, uint32_t events, Callback&& cb);
    //删除io事件
    void RemoveEvent(int fd);
    //获取poller对象
    Poller* GetPoller() { return poller_.get(); };
    //设置ActorSystem（由TcpServer/TcpClient设置）
    void SetActorSystem(ActorSystem* actorSys) { actorSystem_ = actorSys; }
    //获取ActorSystem
    ActorSystem* GetActorSystem() { return actorSystem_; }
    //获取系统毫秒
    int64_t GetMilliSeconds();
    //加入连接对象列表
    void AddConnection(bllsll::IConnection* pConn);
    //获取连接对象列表
    bllsll::IConnection* GetConnection(int fd);
    //删除连接对象（同时移除epoll事件和回调）
    void RemoveConnection(int fd);

private:
    std::unique_ptr<Poller> poller_;
    bool stop_;
    std::thread loopThread_;                        //epoll loop线程
    bllsll::SpinLock cbSpinLock_;                    //保护callbacks_
    std::unordered_map<int, Callback> callbacks_;    //fd→回调映射
    bllsll::SpinLock connSpinLock_;                  //保护mapConn_
    std::unordered_map<int, bllsll::IConnection*> mapConn_; //连接对象
    ActorSystem* actorSystem_ = nullptr;             //Actor系统（不拥有，由外部管理）
};

} //namespace bllsll
