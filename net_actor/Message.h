#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor消息类型定义
*/

#include <string>
#include <cstdint>

namespace bllsll {

enum class MsgType : uint32_t {
    None = 0,
    NetworkRecv,      // 收到网络数据
    Connected,        // 新连接建立
    Disconnected,     // 连接断开
    UserMessage,      // 用户自定义消息
    ActorDown,        // 被 Link 的 Actor 退出/注销（data 中含退出原因）
};

struct ActorMessage {
    MsgType type = MsgType::None;
    uint32_t sourceId = 0;     // 来源actor id (0表示系统/网络消息)
    int fd = -1;               // 关联的fd
    std::string data;          // 消息数据
    uint32_t sessionId = 0;    // 协程会话ID（用于Call/Response配对）
    bool isResponse = false;   // 是否为Call的响应消息

    ActorMessage() = default;
    ActorMessage(MsgType t, uint32_t src, int f, std::string d)
        : type(t), sourceId(src), fd(f), data(std::move(d)) {}
};

} // namespace bllsll
