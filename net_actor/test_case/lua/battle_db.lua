-- 模块：跨服战斗 —— db_service：简单 KV 存储(模拟数据库)
local db = {}

-- 回包判据用 sid ~= 0（有 sessionId 才是 Call，才需要回包）。
-- 注意：不能用 src ~= 0 —— 跨进程消息的 sourceId 恒为 0
-- （见 ClusterProxy.cc: msg.sourceId = 0, "跨进程 sourceId 无意义"），
-- 用 src 判断会导致本服务被 cluster_call 时永不回包，调用方吃满 10s 超时。
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "get" then
        if sid ~= 0 then
            actor.respond(src, sid, {value = db[payload.key] or 0})
        end
    elseif payload.cmd == "set" then
        db[payload.key] = payload.value
        if sid ~= 0 then
            actor.respond(src, sid, {ok = true})
        end
    end
end
