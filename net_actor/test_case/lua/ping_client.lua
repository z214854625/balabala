-- 模块：基础消息 ping/pong —— 客户端
-- 依赖注入的全局：serverId, test_record(cat)  （由 test_actor_lua.cc 注入/注册）
function OnMessage(payload, src, sid, fd)
    if payload.cmd == "start" then
        for i = 1, 10 do
            actor.send(serverId, {cmd="ping", seq=i})
        end
    elseif payload.cmd == "pong" then
        test_record("pong")
    end
end
