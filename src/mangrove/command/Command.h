#pragma once

namespace mangrove::command {

/// 注册客户端指令 `/tweakmenu`，用于强制打开菜单
/// （热键被改坏、或想直接开菜单时用）。
///
/// 在 `Mangrove::enable()` 里调用：指令执行时要能操作已经装好的 ImGui 覆盖层。
void registerMenuCommand();

} // namespace mangrove::command
