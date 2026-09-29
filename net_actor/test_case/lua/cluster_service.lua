-- 模块：跨进程 cluster_call —— 被调服务（本地/跨进程双路回包）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "query" then
        local resp = {result = "processed:" .. payload.data}
        if is_remote then
            actor.respond_remote(src_node, src_name, sid, resp)
        else
            actor.respond(src, sid, resp)
        end
    end
end
