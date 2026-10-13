# 集成移交登记

## 稳定性与安装生命周期修复

| 字段 | 内容 |
| --- | --- |
| 状态 | 待集成负责人接管 |
| 集成负责人 | ShaoLong Wang（GitHub：`sunshine134679`） |
| 分支 | `fix/stability-and-install-lifecycle` |
| 代码提交 | `88a08f3de9c879c99505e00135a6bdd6b7226062` (`fix: harden runtime stability and install lifecycle`) |
| 文件范围 | 40 个修改文件、4 个新增文件；未包含 `modern_ime_personalization/` |
| 专属 worktree | `/home/wsl/.codex/worktrees/stability-install-lifecycle/ModernIME` |

### 自测结果

- `cmake --build --preset fcitx5-debug --parallel 4`：通过。
- `DISPLAY=:1 XAUTHORITY=/run/user/1000/gdm/Xauthority GTK_IM_MODULE=gtk-im-context-simple ctest --preset fcitx5-debug --parallel 1 --output-on-failure`：65 项通过，0 失败、0 跳过。
- `CMAKE_BUILD_PARALLEL_LEVEL=4 ./tests/verify_fresh_clone.sh`：隔离克隆构建、安装、元数据发现与卸载闭环通过；62 项通过，3 项 GTK 测试因沙箱没有显示认证而跳过。GTK 测试已在上面的真实显示环境单独完整通过。
- `git diff --check` 及修改的 shell 脚本 `bash -n` 检查：通过。
- 5 万词条性能基准：索引查询和上下文重排 P99 约 70–93 微秒，通过原有 1 毫秒门槛。

### 接管说明

代码与上述验证已完成，请 ShaoLong Wang（`sunshine134679`）接管评审与后续集成。通知渠道为本仓库的 GitHub 集成移交议题，指派给 `sunshine134679`。

验证结果来自 2026-10-13 的最终代码快照。本次只转移工作区并登记移交，没有修改运行代码。40 个修改文件和 4 个新增文件集中在一个代码提交中；本登记作为独立文档提交引用其 SHA。
