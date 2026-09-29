-- 模块：嵌套协程 run_co —— 调用方
-- 依赖注入的全局：serviceId, test_record(cat)  （由 test_actor_lua_runco.cc 注入/注册）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "trigger" then
        -- actor.run_co 内部启嵌套协程,自动 propagate yield 到 C++
        local result = actor.run_co(function()
            local r1 = actor.call(serviceId, {cmd="query", data=payload.data})
            if not r1 then return {ok = false, reason = "call1 failed"} end
            -- 链式第二次调用,把第一次响应的 result 字段当输入
            local r2 = actor.call(serviceId, {cmd="query", data=r1.result})
            if not r2 then return {ok = false, reason = "call2 failed"} end
            return {ok = true, final = r2.result}
        end)
        if result.ok and result.final == ("processed:processed:" .. payload.data) then
            test_record("ok")
        else
            test_record("bad_resp")
        end
    end
end
