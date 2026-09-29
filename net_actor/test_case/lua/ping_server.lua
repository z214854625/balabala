-- 模块：基础消息 ping/pong —— 服务端
-- 依赖注入的全局：test_record(cat)  （由 test_actor_lua.cc 注册）
function OnMessage(payload, src, sid, fd)
    if payload.cmd == "ping" then
        actor.send(src, {cmd="pong", seq=payload.seq})
        test_record("ping")
    end
end
