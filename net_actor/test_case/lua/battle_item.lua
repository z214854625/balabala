-- 模块：跨服战斗 —— item_service：收到 check_and_deduct 后异步读 DB + 扣减
-- 依赖注入的全局：dbServiceId  （由 test_actor_lua_battle.cc 注入）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "check_and_deduct" then
        -- run_co 内嵌套协程:多个 actor.call 串联(每个都 yield 到 C++)
        local result = actor.run_co(function()
            -- 异步读 DB
            local db_resp = actor.call(dbServiceId, {
                cmd = "get",
                key = payload.player_id .. ":" .. payload.item_id
            })
            if not db_resp then
                return {ok = false, reason = "db read timeout"}
            end

            local current = db_resp.value or 0
            if current < payload.count then
                return {ok = false, reason = "insufficient", has = current, need = payload.count}
            end

            -- 扣减
            local deduct_resp = actor.call(dbServiceId, {
                cmd = "set",
                key = payload.player_id .. ":" .. payload.item_id,
                value = current - payload.count
            })
            if not (deduct_resp and deduct_resp.ok) then
                return {ok = false, reason = "deduct failed"}
            end

            return {ok = true, remaining = current - payload.count}
        end)

        -- 跨进程响应回 nodeA 的 battle_service
        actor.respond_remote(src_node, src_name, sid, result)
    end
end
