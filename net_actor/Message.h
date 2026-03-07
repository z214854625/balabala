#pragma once
/**
@auther: chencaiyu
@date: 2025.3.1
@brief: Actor消息类型定义

改进记录：
  [P2] A.8 消息类型系统：添加 std::any payload 支持类型安全消息传递
  [P3] A.10 优先级消息：添加 priority 字段支持高优先级消息
*/

#include <string>
#include <cstdint>
#include <any>
#include <typeinfo>

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
    std::string data;          // 消息数据（字符串格式，向后兼容）
    uint32_t sessionId = 0;    // 协程会话ID（用于Call/Response配对）
    bool isResponse = false;   // 是否为Call的响应消息
    bool priority = false;     // [P3] A.10 是否为高优先级消息

    // [P2] A.8 类型安全 payload（替代纯字符串传递，可选使用）
    std::any payload;

    ActorMessage() = default;
    ActorMessage(MsgType t, uint32_t src, int f, std::string d)
        : type(t), sourceId(src), fd(f), data(std::move(d)) {}

    // ===== [P2] A.8 类型安全 payload 辅助方法 =====

    // 设置 payload（模板方法）
    template<typename T>
    void SetPayload(T&& value) {
        payload = std::forward<T>(value);
    }

    // 获取 payload（返回指针，失败返回 nullptr）
    template<typename T>
    const T* GetPayload() const {
        return std::any_cast<T>(&payload);
    }

    // 获取 payload（返回指针，可修改版本）
    template<typename T>
    T* GetPayload() {
        return std::any_cast<T>(&payload);
    }

    // 获取 payload（抛异常版本，不推荐用于性能敏感场景）
    template<typename T>
    const T& GetPayloadRef() const {
        return std::any_cast<const T&>(payload);
    }

    // 检查 payload 是否有值
    bool HasPayload() const { return payload.has_value(); }

    // 检查 payload 是否为指定类型
    template<typename T>
    bool IsPayloadType() const {
        return payload.type() == typeid(T);
    }
};

} // namespace bllsll
