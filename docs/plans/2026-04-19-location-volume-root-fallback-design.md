# `--location` 卷根回退兼容设计

## 背景

当前 `--location <folder>` 的解析流程依赖 `GetVolumePathNameW` 返回类似 `C:\` 的盘符根目录。普通本地卷上这个前提成立，但在部分虚拟盘 / RamDisk 驱动上，`GetVolumePathNameW(X:\SomeDir)` 可能错误返回 `X:\SomeDir\`。现有代码会把这种返回值误判成“不是本地盘符”，导致明明位于本地盘符下的子目录也被拒绝。

## 目标

- 保留“只支持盘符根目录卷”的设计约束，不放开到“挂载到文件夹的卷”。
- 兼容 `GetVolumePathNameW` 对部分本地虚拟盘返回异常子目录路径的情况。
- 不影响 `X:` / `X:\` / `C:\Users` 这类现有正常路径。

## 方案

采用“挂载点识别 + 盘符前缀回退”的兼容方案：

1. 仍先对规范化后的 `full_path` 调用 `GetVolumePathNameW`。
2. 若返回值已经是 `X:\` 这种盘符根目录，则保持现有逻辑不变。
3. 若返回值不是盘符根目录，则再调用 `GetVolumeNameForVolumeMountPointW` 判断它是否真的是“挂载到文件夹的卷”。
4. 如果是实际挂载点，继续拒绝，并给出更明确的中文错误。
5. 如果不是实际挂载点，则从 `full_path` 的盘符前缀直接回退为 `X:\`，继续按所属盘符扫描，再用现有 `filter_root` 过滤子树结果。

## 受影响文件

- `src/util.c`：新增挂载点识别与盘符根目录回退逻辑。
- `README.md`：补充说明对异常卷根返回的兼容，以及文件夹挂载卷仍不支持。

## 验证

- `--location C:\Users` 继续成功。
- `--location X:\qq files` 在当前 RamDisk 环境下成功。
- 真正的文件夹挂载卷仍被拒绝，并输出可读中文错误。
- `--location X:` 的现有行为不变。
