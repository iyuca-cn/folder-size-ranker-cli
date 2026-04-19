# `--location` CLI 与中文错误输出设计

## 背景

当前 CLI 使用 `--volume <X:>` 指定扫描盘符。用户希望改为 `--location <path>`，既支持盘符，也支持具体文件夹。当前错误输出使用窄字符 `fprintf` 直接写中文字符串，在 Windows 控制台代码页不匹配时会显示 mojibake。

## 目标

- 将必填参数从 `--volume <X:>` 改为 `--location <path>`。
- `--location` 可以是 `X:`、`X:\` 或某个文件夹路径。
- `--location` 指向 NTFS 子目录时，仍先走整卷 MFT 快路径，再只输出该目录子树内的叶子目录。
- `--volume` 彻底废弃，出现时直接报错提示改用 `--location`。
- 修复中文错误信息乱码。

## 方案

采用“整卷扫描 + 结果过滤”的低风险方案：

1. 参数解析阶段解析 `--location`，用 Windows API 规范化路径并解析所属卷。
2. 内部仍保留 `options->volume`，供 NTFS MFT 扫描和非 NTFS 降级扫描定位卷。
3. 对盘符根目录位置不做结果过滤。
4. 对具体文件夹位置，在 `mftscan_build_results` 构建完整路径后执行大小写不敏感的路径前缀过滤。
5. 错误输出改用 UTF-8 控制台代码页初始化，避免中文字符串被控制台按旧代码页解释。

## 边界

- `--location C:` 和 `--location C:\` 表示扫描整个 `C:`。
- `--location C:\Dir` 只输出 `C:\Dir` 自身或其子树内的叶子目录。
- 非 NTFS 卷仍走平台 API 降级路径；本次先保持扫描整卷再过滤，避免大改扫描器。
- `--volume` 不再作为别名兼容。

## 受影响文件

- `include/mftscan.h`：扩展选项结构和释放函数声明。
- `src/util.c`：参数解析、路径规范化、帮助文本和选项释放。
- `src/main.c`：初始化控制台输出代码页并释放选项。
- `src/aggregate.c`：按 `--location` 子树过滤结果。
- `src/output_json.c`：JSON 元信息从输入位置表达扫描目标。
- `README.md`：更新 CLI 文档和示例。

## 验证

- 构建 Release x64。
- 验证 `--volume` 报错显示中文不乱码。
- 验证 `--location X:` 可以扫描整卷。
- 验证 `--location X:\某目录` 输出路径都在该目录子树内。
