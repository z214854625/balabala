-- 模块：性能基准 —— 被调服务（本地/跨进程双路回包）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "query" then
        if is_remote then
            actor.respond_remote(src_node, src_name, sid, {result = "processed:" .. payload.data})
        else
            actor.respond(src, sid, {result = "processed:" .. payload.data})
        end
    end
end
