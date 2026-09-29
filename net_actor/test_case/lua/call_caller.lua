-- 模块：actor.call 协程融合 —— 调用方
-- 依赖注入的全局：serviceId, test_record(cat)  （由 test_actor_lua_call.cc 注入/注册）
function OnMessage(payload, src, sid, fd)
    if payload.cmd == "trigger" then
        local resp = actor.call(serviceId, {cmd="query", data=payload.data})
        if resp == nil then
            test_record("err")
        elseif resp.result == ("processed:" .. payload.data) then
            test_record("ok")
        else
            test_record("bad_resp")
        end
    end
end
