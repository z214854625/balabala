-- 模块：跨服战斗 —— battle_service：战斗发起前跨服检查道具
-- 依赖注入的全局：test_record(cat)  （由 test_actor_lua_battle.cc 注册）
function OnMessage(payload, src, sid, fd, is_remote, src_node, src_name)
    if payload.cmd == "start_battle" then
        -- run_co 包裹:cluster_call 也是 yield,自动 propagate
        local result = actor.run_co(function()
            -- 战斗前:跨进程检查并扣道具
            local check_resp = actor.cluster_call("nodeB", "item_service", {
                cmd = "check_and_deduct",
                player_id = payload.player_id,
                item_id = payload.item_id,
                count = payload.count
            })

            if not check_resp then
                return {ok = false, reason = "item_service timeout"}
            end

            if not check_resp.ok then
                -- 道具不足,战斗取消
                return {ok = false, reason = check_resp.reason,
                        has = check_resp.has, need = check_resp.need}
            end

            -- 道具扣减成功,执行战斗逻辑
            -- (真实业务这里调战斗系统、伤害计算等)
            local battle_outcome = "victory"

            return {
                ok = true,
                battle = battle_outcome,
                remaining_items = check_resp.remaining
            }
        end)

        if result.ok then
            print("[Battle] player=" .. payload.player_id ..
                  " item=" .. payload.item_id ..
                  " count=" .. payload.count ..
                  " outcome=" .. result.battle ..
                  " remaining=" .. result.remaining_items)
            test_record("ok")
        else
            print("[Battle] player=" .. payload.player_id ..
                  " item=" .. payload.item_id ..
                  " count=" .. payload.count ..
                  " REJECTED reason=" .. (result.reason or "unknown") ..
                  " has=" .. tostring(result.has) ..
                  " need=" .. tostring(result.need))
            test_record("fail")
        end
    end
end
