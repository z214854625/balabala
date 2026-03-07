#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: [P3] A.11 跨进程集群基础框架

设计思路（参考 Skynet cluster 模块）：
  1. 每个进程（节点）有自己的 ActorSystem
  2. ClusterProxy 是一个特殊的本地 Actor，代表远程节点上的 Actor
  3. 当消息发给 ClusterProxy 时，它通过 IClusterTransport 序列化并转发到远程节点
  4. 远程节点收到后，投递给本地目标 Actor

本文件定义：
  - RemoteActorRef：远程 Actor 引用（nodeId + actorId）
  - IClusterTransport：集群传输层接口（TCP/UDP/共享内存等）
  - ClusterProxy：本地代理 Actor
  - ClusterNode：节点信息

注意：这是基础框架定义，提供接口和基本实现。
      完整的网络传输层需要根据实际需求扩展。

使用示例（概念）：
  // 节点A
  auto proxyId = sys.RegisterActor(make_unique<ClusterProxy>("nodeB", 42, &transport));
  sys.Send(proxyId, ActorMessage{...});  // 消息透明转发到节点B的Actor#42

  // 节点B（远程接收端）
  // transport.OnReceive(packet) → sys.Send(42, deserialize(packet));
*/

#include "precompiled.h"
#include "Actor.h"
#include "Message.h"

namespace bllsll {

// ================================================================
//  远程 Actor 引用
// ================================================================

struct RemoteActorRef {
    std::string nodeId;        // 远程节点标识（如 "node_001" 或 "192.168.1.1:9527"）
    uint32_t remoteActorId;    // 远程节点上的 actorId
    std::string remoteName;    // 远程 Actor 名字（可选，用于按名查找）

    RemoteActorRef() : remoteActorId(0) {}
    RemoteActorRef(const std::string& node, uint32_t id)
        : nodeId(node), remoteActorId(id) {}
    RemoteActorRef(const std::string& node, const std::string& name)
        : nodeId(node), remoteActorId(0), remoteName(name) {}

    bool IsValid() const {
        return !nodeId.empty() && (remoteActorId > 0 || !remoteName.empty());
    }

    std::string ToString() const {
        return nodeId + ":" +
               (remoteActorId > 0 ? std::to_string(remoteActorId) : remoteName);
    }
};

// ================================================================
//  集群节点信息
// ================================================================

struct ClusterNode {
    std::string nodeId;        // 节点唯一标识
    std::string address;       // 网络地址（如 "192.168.1.1"）
    int port = 0;              // 端口
    bool alive = false;        // 是否在线

    std::string FullAddress() const {
        return address + ":" + std::to_string(port);
    }
};

// ================================================================
//  集群传输层接口（抽象）
//  实际项目需要实现 TCP/UDP/共享内存等具体传输方式
// ================================================================

struct ClusterPacket {
    std::string sourceNodeId;       // 发送方节点
    std::string targetNodeId;       // 目标节点
    uint32_t sourceActorId = 0;     // 发送方 actorId
    uint32_t targetActorId = 0;     // 目标 actorId
    std::string targetActorName;    // 目标 Actor 名字（按名寻址时使用）
    uint32_t sessionId = 0;         // 会话 ID（用于 Call/Response）
    bool isResponse = false;        // 是否是响应
    std::string data;               // 序列化的消息数据
};

class IClusterTransport
{
public:
    virtual ~IClusterTransport() = default;

    // 发送数据包到远程节点
    virtual bool SendPacket(const ClusterPacket& packet) = 0;

    // 注册本地节点 ID
    virtual void SetLocalNodeId(const std::string& nodeId) = 0;
    virtual std::string GetLocalNodeId() const = 0;

    // 节点发现（可选）
    virtual std::vector<ClusterNode> GetKnownNodes() const { return {}; }
};

// ================================================================
//  本地回环传输层（用于测试，同进程内模拟两个节点）
// ================================================================

class LoopbackTransport : public IClusterTransport
{
public:
    using PacketHandler = std::function<void(const ClusterPacket&)>;

    void SetLocalNodeId(const std::string& nodeId) override {
        localNodeId_ = nodeId;
    }

    std::string GetLocalNodeId() const override {
        return localNodeId_;
    }

    // 注册远程节点的处理器（本地回环：同进程内另一个 ActorSystem 的处理函数）
    void RegisterRemoteHandler(const std::string& nodeId, PacketHandler handler) {
        handlers_[nodeId] = std::move(handler);
    }

    bool SendPacket(const ClusterPacket& packet) override {
        auto it = handlers_.find(packet.targetNodeId);
        if (it != handlers_.end()) {
            it->second(packet);
            return true;
        }
        std::cerr << "[LoopbackTransport] no handler for node: "
                  << packet.targetNodeId << std::endl;
        return false;
    }

private:
    std::string localNodeId_;
    std::unordered_map<std::string, PacketHandler> handlers_;
};

// ================================================================
//  ClusterProxy — 本地代理 Actor
//  每个远程 Actor 在本地创建一个 ClusterProxy 实例。
//  发给 ClusterProxy 的消息会被序列化并通过 transport 转发到远程节点。
// ================================================================

class ClusterProxy : public Actor
{
public:
    ClusterProxy(const RemoteActorRef& remote, IClusterTransport* transport)
        : remote_(remote), transport_(transport)
    {
    }

    // 获取远程引用信息
    const RemoteActorRef& GetRemoteRef() const { return remote_; }

    void OnMessage(ActorMessage& msg) override
    {
        if (!transport_) {
            std::cerr << "[ClusterProxy] no transport! actorId=" << GetActorId()
                      << ", remote=" << remote_.ToString() << std::endl;
            return;
        }

        // 构造集群数据包
        ClusterPacket packet;
        packet.sourceNodeId = transport_->GetLocalNodeId();
        packet.targetNodeId = remote_.nodeId;
        packet.sourceActorId = msg.sourceId;
        packet.targetActorId = remote_.remoteActorId;
        packet.targetActorName = remote_.remoteName;
        packet.sessionId = msg.sessionId;
        packet.isResponse = msg.isResponse;
        packet.data = msg.data;  // 简单场景直接传 string；实际项目需要序列化 payload

        bool ok = transport_->SendPacket(packet);
        if (!ok) {
            std::cerr << "[ClusterProxy] SendPacket failed! remote="
                      << remote_.ToString() << std::endl;
        }
    }

private:
    RemoteActorRef remote_;
    IClusterTransport* transport_;  // 不拥有，由外部管理
};

// ================================================================
//  ClusterReceiver — 远程消息接收处理器（辅助类）
//  注册为 LoopbackTransport 的 handler，收到远程包后投递给本地 ActorSystem
// ================================================================

class ActorSystem;  // 前向声明

class ClusterReceiver
{
public:
    ClusterReceiver(ActorSystem* sys) : sys_(sys) {}

    // 处理收到的远程数据包
    void OnPacketReceived(const ClusterPacket& packet);

private:
    ActorSystem* sys_;
};

} // namespace bllsll
