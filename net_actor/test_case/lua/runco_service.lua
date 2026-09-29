-- 模块：嵌套协程 run_co —— 被调服务
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "query" then
        actor.respond(src, sid, {result = "processed:" .. payload.data})
    end
end
