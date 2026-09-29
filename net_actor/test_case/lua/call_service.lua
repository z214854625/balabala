-- 模块：actor.call 协程融合 —— 被调服务
function OnMessage(payload, src, sid, fd)
    if payload.cmd == "query" then
        actor.respond(src, sid, {result = "processed:" .. payload.data})
    end
end
