#pragma once
/**
@auther: chencaiyu
@date: 2025.3
@brief: [P3] A.11 跨进程集群框架

设计思路（参考 Skynet cluster 模块）：
  1. 每个进程（节点）有自己的 ActorSystem + EventLoop
  2. ClusterProxy 是一个特殊的本地 Actor，代表远程节点上的 Actor
  3. 当消息发给 ClusterProxy 时，它通过 IClusterTransport 序列化并转发到远程节点
  4. 远程节点收到后，投递给本地目标 Actor

本文件定义：
  - RemoteActorRef：远程 Actor 引用（nodeId + actorId/name）
  - IClusterTransport：集群传输层接口
  - LoopbackTransport：本地回环传输（用于单元测试）
  - ClusterProxy：本地代理 Actor
  - ClusterReceiver：远程消息接收处理器
  - ClusterPacketCodec：二进制序列化/反序列化（长度前缀协议）
  - ClusterGatewayActor：集群 TCP I/O 处理 Actor（TCP 流重组）
  - TcpClusterTransport：基于 TCP 的真实跨网络/跨进程传输

线协议（Wire Protocol）：
  [4 bytes: body_length, big-endian]
  [body]:
    [2 bytes: sourceNodeId.len][sourceNodeId]
    [2 bytes: targetNodeId.len][targetNodeId]
    [4 bytes: sourceActorId]
    [4 bytes: targetActorId]
    [2 bytes: targetActorName.len][targetActorName]
    [4 bytes: sessionId]
    [1 byte: flags (bit0=isResponse)]
    [4 bytes: data.len][data]

使用示例（真实 TCP）：
  // ---- 节点 B（服务端） ----
  EventLoop loopB;  loopB.Create();
  ActorSystem sysB; sysB.Start(4, &loopB); loopB.SetActorSystem(&sysB);
  TcpClusterTransport transportB(&sysB, &loopB);
  transportB.SetLocalNodeId("nodeB");
  transportB.Listen(9600);  // 监听集群端口
  auto echoId = sysB.RegisterActor(make_unique<EchoActor>());
  sysB.RegisterName("echo_service", echoId);

  // ---- 节点 A（客户端） ----
  EventLoop loopA;  loopA.Create();
  ActorSystem sysA; sysA.Start(4, &loopA); loopA.SetActorSystem(&sysA);
  TcpClusterTransport transportA(&sysA, &loopA);
  transportA.SetLocalNodeId("nodeA");
  transportA.ConnectToNode("nodeB", "192.168.1.2", 9600);

  // 创建代理 → 透明转发到节点 B
  RemoteActorRef ref("nodeB", "echo_service");
  auto proxyId = sysA.RegisterActor(make_unique<ClusterProxy>(ref, &transportA));
  sysA.Send(proxyId, ActorMessage{MsgType::UserMessage, 0, -1, "hello cluster!"});
*/

#include "precompiled.h"
#include "Actor.h"
#include "Message.h"
#include "../Util/SpinLock.h"
#include "../Util/LockGuard.h"

namespace bllsll {

class EventLoop;
class Acceptor;

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
//  集群数据包
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

// ================================================================
//  ClusterPacketCodec — 二进制序列化/反序列化
//
//  线协议：[4 bytes: body_length][body_bytes]
//  支持 TCP 流重组（部分包/粘包处理）
// ================================================================

class ClusterPacketCodec
{
public:
    // 序列化 ClusterPacket → 带长度前缀的二进制数据
    static std::string Encode(const ClusterPacket& pkt);

    // 尝试从缓冲区解码一个完整数据包
    // 返回值 > 0：消耗的字节数（成功解码）
    // 返回值 == 0：缓冲区不完整，需要更多数据
    static size_t Decode(const char* data, size_t len, ClusterPacket& out);

private:
    // 写入辅助
    static void WriteU8(std::string& buf, uint8_t v);
    static void WriteU16(std::string& buf, uint16_t v);
    static void WriteU32(std::string& buf, uint32_t v);
    static void WriteStr16(std::string& buf, const std::string& s);
    static void WriteStr32(std::string& buf, const std::string& s);

    // 读取辅助（p 自动前进，越界返回 false）
    static uint32_t PeekU32(const char* p);
    static bool ReadU8(const char*& p, const char* end, uint8_t& v);
    static bool ReadU32(const char*& p, const char* end, uint32_t& v);
    static bool ReadStr16(const char*& p, const char* end, std::string& s);
    static bool ReadStr32(const char*& p, const char* end, std::string& s);
};

// ================================================================
//  集群传输层接口（抽象）
// ================================================================

class IClusterTransport
{
public:
    virtual ~IClusterTransport() = default;

    // 发送数据包到远程节点
    virtual bool SendPacket(const ClusterPacket& packet) = 0;

    // 本地节点 ID
    virtual void SetLocalNodeId(const std::string& nodeId) = 0;
    virtual std::string GetLocalNodeId() const = 0;

    // 节点发现（可选）
    virtual std::vector<ClusterNode> GetKnownNodes() const { return {}; }
};

// ================================================================
//  LoopbackTransport — 本地回环传输（用于单元测试）
//  同进程内模拟两个节点，不经过网络
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

    // 注册远程节点的处理器
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

    const RemoteActorRef& GetRemoteRef() const { return remote_; }

    void OnMessage(ActorMessage& msg) override
    {
        if (!transport_) {
            std::cerr << "[ClusterProxy] no transport! actorId=" << GetActorId()
                      << ", remote=" << remote_.ToString() << std::endl;
            return;
        }

        ClusterPacket packet;
        packet.sourceNodeId = transport_->GetLocalNodeId();
        packet.targetNodeId = remote_.nodeId;
        packet.sourceActorId = msg.sourceId;
        packet.targetActorId = remote_.remoteActorId;
        packet.targetActorName = remote_.remoteName;
        packet.sessionId = msg.sessionId;
        packet.isResponse = msg.isResponse;
        packet.data = msg.data;

        bool ok = transport_->SendPacket(packet);
        if (!ok) {
            std::cerr << "[ClusterProxy] SendPacket failed! remote="
                      << remote_.ToString() << std::endl;
        }
    }

private:
    RemoteActorRef remote_;
    IClusterTransport* transport_;
};

// ================================================================
//  ClusterReceiver — 远程消息接收处理器（用于 LoopbackTransport）
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

// ================================================================
//  TcpClusterTransport — 基于 TCP 的真实跨网络/跨进程集群传输
//
//  核心设计：
//    - 内部创建 ClusterGatewayActor 处理集群 TCP I/O
//    - Listen(port) 创建 Acceptor 接受远程节点连接
//    - ConnectToNode() 创建 Connector 连接远程节点
//    - 连接建立后自动发送握手包（携带 localNodeId）
//    - 握手完成后 fd ↔ nodeId 映射建立
//    - SendPacket() 将 ClusterPacket 序列化为二进制，通过 TCP 发送
//    - 收到 TCP 数据后，ClusterGatewayActor 进行流重组 + 解码
//    - 解码出的 ClusterPacket 路由到本地 ActorSystem
//
//  线程安全：
//    - nodeToFd_/fdToNode_ 用 SpinLock 保护
//    - recvBuffers_ 在 GatewayActor 的 OnMessage 中串行访问
//    - Connection::Send() 本身是线程安全的（sendMQ_ + epoll_ctl）
// ================================================================

class TcpClusterTransport : public IClusterTransport
{
public:
    TcpClusterTransport(ActorSystem* sys, EventLoop* loop);
    ~TcpClusterTransport();

    // 监听集群端口（接受远程节点的连接）
    void Listen(int port);

    // 主动连接远程节点
    void ConnectToNode(const std::string& nodeId, const std::string& ip, int port);

    // ===== IClusterTransport 接口 =====
    bool SendPacket(const ClusterPacket& packet) override;
    void SetLocalNodeId(const std::string& nodeId) override;
    std::string GetLocalNodeId() const override;
    std::vector<ClusterNode> GetKnownNodes() const override;

    // ===== 内部接口（由 ClusterGatewayActor 调用） =====
    // 注册 fd → nodeId 映射（握手完成后调用）
    void RegisterNodeFd(const std::string& nodeId, int fd);
    // 获取 nodeId 对应的 fd（-1 表示未连接）
    int GetFdByNodeId(const std::string& nodeId);
    // 获取 fd 对应的 nodeId（空串表示未知）
    std::string GetNodeIdByFd(int fd);
    // fd 断开时清理映射
    void OnNodeDisconnected(int fd);
    // 获取 ActorSystem
    ActorSystem* GetActorSystem() { return sys_; }
    // 获取 EventLoop
    EventLoop* GetEventLoop() { return loop_; }
    // 获取 GatewayActor ID
    uint32_t GetGatewayActorId() const { return gatewayActorId_; }

    // ClusterGatewayActor 需要调用 sendHandshake
    friend class ClusterGatewayActor;

private:
    // 发送握手包（连接建立后调用）
    void sendHandshake(int fd);

    ActorSystem* sys_;
    EventLoop* loop_;
    std::string localNodeId_;
    uint32_t gatewayActorId_ = 0;
    Acceptor* acceptor_ = nullptr;

    // fd ↔ nodeId 双向映射（SpinLock 保护）
    bllsll::SpinLock connLock_;
    std::unordered_map<std::string, int> nodeToFd_;
    std::unordered_map<int, std::string> fdToNode_;

    // 已知节点列表
    mutable bllsll::SpinLock nodesLock_;
    std::vector<ClusterNode> knownNodes_;

    // 待握手连接（ConnectToNode 时预存 fd → nodeId）
    bllsll::SpinLock pendingLock_;
    std::unordered_map<int, std::string> pendingNodeId_;
};

// ================================================================
//  ClusterGatewayActor — 集群 TCP I/O 处理 Actor
//
//  职责：
//    1. 处理 Connected：发送握手包
//    2. 处理 NetworkRecv：TCP 流重组 + 解码 ClusterPacket
//       - 握手包：提取 sourceNodeId，建立 fd ↔ nodeId 映射
//       - 数据包：路由到本地 ActorSystem
//    3. 处理 Disconnected：清理连接映射
//
//  TCP 流重组：
//    - 每个 fd 维护一个接收缓冲区（recvBuffers_）
//    - 新数据追加到缓冲区
//    - 循环尝试解码完整的 ClusterPacket
//    - 直到缓冲区不足一个完整包
// ================================================================

class ClusterGatewayActor : public Actor
{
public:
    ClusterGatewayActor(TcpClusterTransport* transport)
        : transport_(transport)
    {
    }

    void OnMessage(ActorMessage& msg) override;

private:
    // 处理一个解码后的集群数据包
    void handleDecodedPacket(const ClusterPacket& pkt, int fd);

    TcpClusterTransport* transport_;
    // per-fd 接收缓冲区（仅在 OnMessage 中串行访问，无需锁）
    std::unordered_map<int, std::string> recvBuffers_;
};

} // namespace bllsll
