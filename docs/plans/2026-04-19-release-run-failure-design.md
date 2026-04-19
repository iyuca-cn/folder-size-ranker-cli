# GitHub Release 工作流失败修复设计

## 背景

`v1.0.1` 触发的 Release 工作流在 `Read annotated tag notes` 步骤失败。公开 job 元数据表明失败点位于该步骤，对应当前工作流里通过 `System.Diagnostics.ProcessStartInfo` / `Process.Start()` 包装 `git` 的自定义读取逻辑。

本地确认：

- `v1.0.1` 是 annotated tag。
- `v1.0.1` 的说明非空。

因此失败根因不在 tag 本身，而在工作流实现过度复杂，在 GitHub runner 环境下不稳定。

## 目标

- 保持原有规则不变：
  - 只能接受 annotated tag。
  - annotated tag 说明不能为空。
  - GitHub Release 正文严格使用 annotated tag 的完整说明全文。
- 去掉 runner 上不稳定的 `ProcessStartInfo` / `Process.Start()` 包装逻辑。

## 方案

采用“简单原生命令 + 重定向到临时文件 + 读回全文”的方案：

1. 继续用 `git cat-file -t` 校验 `refs/tags/<tag>` 的对象类型必须是 `tag`。
2. 使用 `Start-Process` 调用 `git for-each-ref <tagRef> --format=%(contents)`。
3. 通过 `-RedirectStandardOutput` 把 git 原始输出写到临时文件。
4. 用 .NET 文件 API 读取临时文件全文。
5. 如果说明为空或全空白，直接失败。
6. 继续通过多行 `GITHUB_OUTPUT` 把正文传给 `softprops/action-gh-release` 的 `body`。

## 为什么这样更稳

- 避免手写 `ProcessStartInfo.ArgumentList` 和 `Process.Start()` 生命周期管理。
- `Start-Process -RedirectStandardOutput` 在 GitHub Windows runner 上兼容性更好。
- 通过文件读取可以保留完整多行正文，并且逻辑更易调试。

## 受影响文件

- `.github/workflows/release.yml`
- `README.md`

## 验证

- 本地确认 annotated tag `v1.0.1` 仍能被识别为 annotated tag。
- 本地确认 lightweight tag 仍会被拒绝。
- 本地确认工作流中不再使用 `ProcessStartInfo` / `Process.Start()` 自定义 git 包装。
