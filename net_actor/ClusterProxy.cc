#include "ClusterProxy.h"
#include "ActorSystem.h"
#include "EventLoop.h"
#include "Acceptor.h"
#include "Connector.h"

using namespace bllsll;
using namespace std;

// ================================================================
//  ClusterProxy::OnMessage — 本地代理 Actor
//  收到消息后，通过 transport 转发到远程节点
// ================================================================

ActorTask ClusterProxy::OnCoroutineMessage(ActorMessage msg)
{
    if (!transport_) co_return;

    // 自动查找发送方 Actor 的注册名字（用于回程路由）
    std::string senderName;
    if (system_ && msg.sourceId > 0) {
        senderName = system_->GetActorName(msg.sourceId);
    }

    ClusterPacket pkt;
    pkt.sourceNodeId = transport_->GetLocalNodeId();
    pkt.targetNodeId = remote_.nodeId;
    pkt.sourceActorName = senderName;
    pkt.targetActorName = remote_.remoteName;
    pkt.sessionId = msg.sessionId;
    pkt.isResponse = msg.isResponse;
    pkt.data = msg.data;

    if (!transport_->SendPacket(pkt)) {
        std::cerr << "[ClusterProxy] failed to send to " << remote_.ToString() << std::endl;
    }
    co_return;
}

// ================================================================
//  ClusterReceiver — LoopbackTransport 用
// ================================================================

void ClusterReceiver::OnPacketReceived(const ClusterPacket& packet)
{
    if (!sys_) return;

    ActorMessage msg;
    msg.type = MsgType::UserMessage;
    msg.sourceId = 0;  // 跨进程 sourceId 无意义
    msg.sessionId = packet.sessionId;
    msg.isResponse = packet.isResponse;
    msg.data = packet.data;
    // 设置跨进程路由信息（接收方可通过 IsRemote()/RespondRemote() 使用）
    msg.sourceNodeId = packet.sourceNodeId;
    msg.sourceActorName = packet.sourceActorName;

    // 跨进程只能按名字寻址
    if (!packet.targetActorName.empty()) {
        sys_->SendByName(packet.targetActorName, std::move(msg));
    } else {
        std::cerr << "[ClusterReceiver] packet has no targetActorName, from="
                  << packet.sourceNodeId << std::endl;
    }
}

// ================================================================
//  ClusterPacketCodec — 二进制序列化/反序列化
// ================================================================

void ClusterPacketCodec::WriteU8(std::string& buf, uint8_t v)
{
    buf.push_back(static_cast<char>(v));
}

void ClusterPacketCodec::WriteU16(std::string& buf, uint16_t v)
{
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<char>(v & 0xFF));
}

void ClusterPacketCodec::WriteU32(std::string& buf, uint32_t v)
{
    buf.push_back(static_cast<char>((v >> 24) & 0xFF));
    buf.push_back(static_cast<char>((v >> 16) & 0xFF));
    buf.push_back(static_cast<char>((v >> 8) & 0xFF));
    buf.push_back(static_cast<char>(v & 0xFF));
}

void ClusterPacketCodec::WriteStr16(std::string& buf, const std::string& s)
{
    WriteU16(buf, static_cast<uint16_t>(s.size()));
    buf.append(s);
}

void ClusterPacketCodec::WriteStr32(std::string& buf, const std::string& s)
{
    WriteU32(buf, static_cast<uint32_t>(s.size()));
    buf.append(s);
}

uint32_t ClusterPacketCodec::PeekU32(const char* p)
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])));
}

bool ClusterPacketCodec::ReadU8(const char*& p, const char* end, uint8_t& v)
{
    if (p + 1 > end) return false;
    v = static_cast<uint8_t>(*p);
    p += 1;
    return true;
}

bool ClusterPacketCodec::ReadU32(const char*& p, const char* end, uint32_t& v)
{
    if (p + 4 > end) return false;
    v = PeekU32(p);
    p += 4;
    return true;
}

bool ClusterPacketCodec::ReadStr16(const char*& p, const char* end, std::string& s)
{
    if (p + 2 > end) return false;
    uint16_t len = (static_cast<uint16_t>(static_cast<uint8_t>(p[0])) << 8) |
                    static_cast<uint16_t>(static_cast<uint8_t>(p[1]));
    p += 2;
    if (p + len > end) return false;
    s.assign(p, len);
    p += len;
    return true;
}

bool ClusterPacketCodec::ReadStr32(const char*& p, const char* end, std::string& s)
{
    if (p + 4 > end) return false;
    uint32_t len = PeekU32(p);
    p += 4;
    if (p + len > end) return false;
    s.assign(p, len);
    p += len;
    return true;
}

std::string ClusterPacketCodec::Encode(const ClusterPacket& pkt)
{
    // 先编码 body（v2 协议：移除 actorId 字段，添加 sourceActorName）
    std::string body;
    body.reserve(128);

    WriteStr16(body, pkt.sourceNodeId);
    WriteStr16(body, pkt.targetNodeId);
    WriteStr16(body, pkt.sourceActorName);    // v2: 发送方 Actor 名字
    WriteStr16(body, pkt.targetActorName);    // 目标 Actor 名字
    WriteU32(body, pkt.sessionId);
    WriteU8(body, pkt.isResponse ? 1 : 0);
    WriteStr32(body, pkt.data);

    // 加上长度前缀
    std::string result;
    result.reserve(4 + body.size());
    WriteU32(result, static_cast<uint32_t>(body.size()));
    result.append(body);

    return result;
}

size_t ClusterPacketCodec::Decode(const char* data, size_t len, ClusterPacket& out)
{
    // 至少需要 4 字节的长度头
    if (len < 4) return 0;

    uint32_t bodyLen = PeekU32(data);
    // 防止恶意超大包（最大 16MB）
    if (bodyLen > 16 * 1024 * 1024) {
        std::cerr << "[ClusterPacketCodec] packet too large: " << bodyLen << std::endl;
        return 0;
    }

    // 检查是否有足够数据
    if (len < 4 + bodyLen) return 0;

    const char* p = data + 4;
    const char* end = data + 4 + bodyLen;

    // v2 协议：移除 actorId 字段，添加 sourceActorName
    if (!ReadStr16(p, end, out.sourceNodeId)) return 0;
    if (!ReadStr16(p, end, out.targetNodeId)) return 0;
    if (!ReadStr16(p, end, out.sourceActorName)) return 0;    // v2
    if (!ReadStr16(p, end, out.targetActorName)) return 0;
    if (!ReadU32(p, end, out.sessionId)) return 0;

    uint8_t flags = 0;
    if (!ReadU8(p, end, flags)) return 0;
    out.isResponse = (flags & 1) != 0;

    if (!ReadStr32(p, end, out.data)) return 0;

    return 4 + bodyLen;
}

// ================================================================
//  ClusterGatewayActor — 集群 TCP I/O 处理
// ================================================================

static const std::string CLUSTER_HANDSHAKE_MAGIC = "CLUSTER_HANDSHAKE";

ActorTask ClusterGatewayActor::OnCoroutineMessage(ActorMessage msg)
{
    switch (msg.type) {
    case MsgType::Connected: {
        std::cout << "[ClusterGateway] connection established, fd=" << msg.fd << std::endl;
        // 初始化接收缓冲区
        recvBuffers_[msg.fd] = "";
        // 发送握手包（无论是主动连接还是被动接受）
        transport_->sendHandshake(msg.fd);
        break;
    }

    case MsgType::NetworkRecv: {
        // 追加到接收缓冲区
        auto& buf = recvBuffers_[msg.fd];
        buf.append(msg.data);

        // 循环尝试解码完整数据包
        while (true) {
            ClusterPacket pkt;
            size_t consumed = ClusterPacketCodec::Decode(buf.data(), buf.size(), pkt);
            if (consumed == 0) {
                // 数据不完整，等待更多数据
                break;
            }
            // 处理解码后的数据包
            handleDecodedPacket(pkt, msg.fd);
            // 移除已消耗的数据
            buf.erase(0, consumed);
        }
        break;
    }

    case MsgType::Disconnected: {
        std::cout << "[ClusterGateway] connection lost, fd=" << msg.fd << std::endl;
        // 清理接收缓冲区
        recvBuffers_.erase(msg.fd);
        // 通知 transport 清理映射
        transport_->OnNodeDisconnected(msg.fd);
        break;
    }

    default:
        break;
    }
    co_return;
}

void ClusterGatewayActor::handleDecodedPacket(const ClusterPacket& pkt, int fd)
{
    // 检查是否是握手包
    if (pkt.data == CLUSTER_HANDSHAKE_MAGIC) {
        std::string remoteNodeId = pkt.sourceNodeId;
        if (remoteNodeId.empty()) {
            std::cerr << "[ClusterGateway] handshake missing sourceNodeId, fd=" << fd << std::endl;
            return;
        }
        // 注册 fd → nodeId 映射
        transport_->RegisterNodeFd(remoteNodeId, fd);
        std::cout << "[ClusterGateway] handshake completed: nodeId=" << remoteNodeId
                  << ", fd=" << fd << std::endl;
        return;
    }

    // 普通数据包 → 路由到本地 ActorSystem
    auto* sys = transport_->GetActorSystem();
    if (!sys) return;

    ActorMessage actorMsg;
    actorMsg.type = MsgType::UserMessage;
    actorMsg.sourceId = 0;  // 跨进程 sourceId 无意义
    actorMsg.sessionId = pkt.sessionId;
    actorMsg.isResponse = pkt.isResponse;
    actorMsg.data = pkt.data;
    // 设置跨进程路由信息（接收方可通过 IsRemote()/RespondRemote() 使用）
    actorMsg.sourceNodeId = pkt.sourceNodeId;
    actorMsg.sourceActorName = pkt.sourceActorName;

    // 跨进程只能按名字寻址
    if (!pkt.targetActorName.empty()) {
        bool sent = sys->SendByName(pkt.targetActorName, std::move(actorMsg));
        if (!sent) {
            std::cerr << "[ClusterGateway] target actor name not found: "
                      << pkt.targetActorName << std::endl;
        }
    } else {
        std::cerr << "[ClusterGateway] packet has no targetActorName, from="
                  << pkt.sourceNodeId << std::endl;
    }
}

// ================================================================
//  TcpClusterTransport — 基于 TCP 的真实集群传输
// ================================================================

TcpClusterTransport::TcpClusterTransport(ActorSystem* sys, EventLoop* loop)
    : sys_(sys), loop_(loop)
{
    // 创建并注册内部的 ClusterGatewayActor
    auto gateway = std::make_unique<ClusterGatewayActor>(this);
    gatewayActorId_ = sys_->RegisterActor(std::move(gateway));
    std::cout << "[TcpClusterTransport] created, gatewayActorId=" << gatewayActorId_ << std::endl;
}

TcpClusterTransport::~TcpClusterTransport()
{
    // 注销 GatewayActor
    if (sys_ && gatewayActorId_ > 0) {
        sys_->UnregisterActor(gatewayActorId_);
    }
}

void TcpClusterTransport::SetLocalNodeId(const std::string& nodeId)
{
    localNodeId_ = nodeId;
}

std::string TcpClusterTransport::GetLocalNodeId() const
{
    return localNodeId_;
}

void TcpClusterTransport::Listen(int port)
{
    // 创建 Acceptor，新连接绑定到 ClusterGatewayActor
    acceptor_ = new Acceptor(port, loop_, gatewayActorId_);
    std::cout << "[TcpClusterTransport] listening on port " << port
              << " (gatewayActorId=" << gatewayActorId_ << ")" << std::endl;
}

void TcpClusterTransport::ConnectToNode(const std::string& nodeId,
                                         const std::string& ip, int port)
{
    // 创建 Connector，连接结果通知给 ClusterGatewayActor
    auto* connector = new Connector(loop_, port, ip, gatewayActorId_);
    int fd = connector->GetFd();

    // 预存 fd → nodeId（Connector 构造时 socket 已创建）
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        pendingNodeId_[fd] = nodeId;
    }

    // 记录已知节点
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(nodesLock_);
        ClusterNode node;
        node.nodeId = nodeId;
        node.address = ip;
        node.port = port;
        node.alive = false;  // 尚未完成握手
        knownNodes_.push_back(node);
    }

    std::cout << "[TcpClusterTransport] connecting to node=" << nodeId
              << " (" << ip << ":" << port << "), fd=" << fd << std::endl;
}

bool TcpClusterTransport::SendPacket(const ClusterPacket& packet)
{
    // 查找目标节点的 fd
    int fd = GetFdByNodeId(packet.targetNodeId);
    if (fd < 0) {
        std::cerr << "[TcpClusterTransport] node not connected: "
                  << packet.targetNodeId << std::endl;
        return false;
    }

    // 序列化
    std::string encoded = ClusterPacketCodec::Encode(packet);

    // 通过 EventLoop 的 Connection 发送
    auto* conn = loop_->GetConnection(fd);
    if (!conn) {
        std::cerr << "[TcpClusterTransport] connection lost for node="
                  << packet.targetNodeId << ", fd=" << fd << std::endl;
        return false;
    }

    conn->Send(encoded.data(), static_cast<int>(encoded.size()));
    return true;
}

std::vector<ClusterNode> TcpClusterTransport::GetKnownNodes() const
{
    bllsll::LockGuard<bllsll::SpinLock> lock(nodesLock_);
    return knownNodes_;
}

void TcpClusterTransport::RegisterNodeFd(const std::string& nodeId, int fd)
{
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(connLock_);
        nodeToFd_[nodeId] = fd;
        fdToNode_[fd] = nodeId;
    }

    // 从 pending 中移除
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        pendingNodeId_.erase(fd);
    }

    // 更新已知节点状态
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(nodesLock_);
        for (auto& node : knownNodes_) {
            if (node.nodeId == nodeId) {
                node.alive = true;
                break;
            }
        }
    }

    std::cout << "[TcpClusterTransport] node registered: " << nodeId
              << ", fd=" << fd << std::endl;
}

int TcpClusterTransport::GetFdByNodeId(const std::string& nodeId)
{
    bllsll::LockGuard<bllsll::SpinLock> lock(connLock_);
    auto it = nodeToFd_.find(nodeId);
    if (it != nodeToFd_.end()) {
        return it->second;
    }
    return -1;
}

std::string TcpClusterTransport::GetNodeIdByFd(int fd)
{
    // 先查已确认的映射
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(connLock_);
        auto it = fdToNode_.find(fd);
        if (it != fdToNode_.end()) {
            return it->second;
        }
    }
    // 再查 pending（ConnectToNode 时预存的）
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        auto it = pendingNodeId_.find(fd);
        if (it != pendingNodeId_.end()) {
            return it->second;
        }
    }
    return "";
}

void TcpClusterTransport::OnNodeDisconnected(int fd)
{
    std::string nodeId;
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(connLock_);
        auto it = fdToNode_.find(fd);
        if (it != fdToNode_.end()) {
            nodeId = it->second;
            fdToNode_.erase(it);
            nodeToFd_.erase(nodeId);
        }
    }
    {
        bllsll::LockGuard<bllsll::SpinLock> lock(pendingLock_);
        pendingNodeId_.erase(fd);
    }

    if (!nodeId.empty()) {
        // 更新已知节点状态
        bllsll::LockGuard<bllsll::SpinLock> lock(nodesLock_);
        for (auto& node : knownNodes_) {
            if (node.nodeId == nodeId) {
                node.alive = false;
                break;
            }
        }
        std::cout << "[TcpClusterTransport] node disconnected: " << nodeId
                  << ", fd=" << fd << std::endl;
    }
}

void TcpClusterTransport::sendHandshake(int fd)
{
    if (localNodeId_.empty()) {
        std::cerr << "[TcpClusterTransport] cannot send handshake: localNodeId not set!"
                  << std::endl;
        return;
    }

    // 构造握手包（v2 协议：无 actorId 字段）
    ClusterPacket handshake;
    handshake.sourceNodeId = localNodeId_;
    handshake.targetNodeId = "";  // 对方还不知道
    handshake.sourceActorName = "";  // 握手无需 Actor 名字
    handshake.targetActorName = "";
    handshake.sessionId = 0;
    handshake.isResponse = false;
    handshake.data = CLUSTER_HANDSHAKE_MAGIC;

    // 序列化并发送
    std::string encoded = ClusterPacketCodec::Encode(handshake);
    auto* conn = loop_->GetConnection(fd);
    if (conn) {
        conn->Send(encoded.data(), static_cast<int>(encoded.size()));
        std::cout << "[TcpClusterTransport] handshake sent, fd=" << fd
                  << ", localNodeId=" << localNodeId_ << std::endl;
    } else {
        std::cerr << "[TcpClusterTransport] handshake failed: no connection for fd="
                  << fd << std::endl;
    }
}
