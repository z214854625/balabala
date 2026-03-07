#include "ClusterProxy.h"
#include "ActorSystem.h"

using namespace bllsll;

// ================================================================
//  ClusterReceiver 实现
//  收到远程数据包后，投递给本地 ActorSystem
// ================================================================

void ClusterReceiver::OnPacketReceived(const ClusterPacket& packet)
{
    if (!sys_) {
        std::cerr << "[ClusterReceiver] no ActorSystem!" << std::endl;
        return;
    }

    // 重建 ActorMessage
    ActorMessage msg;
    msg.type = MsgType::UserMessage;
    msg.sourceId = packet.sourceActorId;
    msg.fd = -1;
    msg.data = packet.data;
    msg.sessionId = packet.sessionId;
    msg.isResponse = packet.isResponse;

    // 按 actorId 或按名字投递
    if (packet.targetActorId > 0) {
        sys_->Send(packet.targetActorId, std::move(msg));
    } else if (!packet.targetActorName.empty()) {
        sys_->SendByName(packet.targetActorName, std::move(msg));
    } else {
        std::cerr << "[ClusterReceiver] packet has no target! from node="
                  << packet.sourceNodeId << std::endl;
    }
}
