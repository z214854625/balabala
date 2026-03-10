#pragma once
/**
@brief: 向后兼容别名 — CoroutineActor 已合并入 Actor
        保留此头文件仅为兼容已有 #include "CoroutineActor.h" 的代码
*/

#include "Actor.h"

namespace bllsll {

// CoroutineActor 的所有功能已合并到 Actor 中
// 此别名保证旧代码 class MyActor : public CoroutineActor 仍可编译
using CoroutineActor = Actor;

} // namespace bllsll
