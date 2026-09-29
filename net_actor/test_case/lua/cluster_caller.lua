-- 模块：跨进程 cluster_call —— 调用方
-- 依赖注入的全局：test_record(cat)  （由 test_actor_lua_cluster.cc 注册）
-- 目标服务按 node/name 寻址（"nodeB" / "lua_service"），无需注入 id
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "trigger" then
        local resp = actor.cluster_call("nodeB", "lua_service",
                                        {cmd="query", data=payload.data})
        if resp == nil then
            test_record("err")
        elseif resp.result == ("processed:" .. payload.data) then
            test_record("ok")
        else
            test_record("bad_resp")
        end
    end
end
