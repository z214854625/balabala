-- 模块：性能基准 —— Lua 调用方
-- 依赖注入的全局：serviceId, test_record(cat)  （由 test_actor_lua_bench.cc 注入/注册）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "trigger" then
        for i = 1, payload.n do
            local resp = actor.call(serviceId, {cmd="query", data="bench"})
            if not resp or resp.result ~= "processed:bench" then
                test_record("err")
            end
        end
        test_record("done")
    end
end
