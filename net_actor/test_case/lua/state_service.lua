-- 模块：actor 内共享状态（串行不变量保证无锁安全）
local state = {}   -- 本 actor 进程内共享状态（串行不变量保证无锁安全）

function OnMessage(payload, src, sid, fd)
    if payload.cmd == "set_and_get" then
        state[payload.key] = payload.value
        local got = state[payload.key]
        actor.respond(src, sid, {
            key = payload.key,
            value = got,
            expected = payload.value,
            ok = (got == payload.value)
        })
    elseif payload.cmd == "dump" then
        -- 调试用：返回所有键
        local keys = {}
        for k, _ in pairs(state) do
            keys[#keys + 1] = k
        end
        actor.respond(src, sid, {keys = keys})
    else
        actor.respond(src, sid, {ok = false, reason = "unknown cmd"})
    end
end
