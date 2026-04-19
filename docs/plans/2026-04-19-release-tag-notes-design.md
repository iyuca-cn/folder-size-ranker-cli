# GitHub Release 正文与 Annotated Tag 说明一致性设计

## 背景

当前 `release.yml` 在发布时会读取 tag 说明写入 `release-notes.md`，但当读取不到内容时会回退为 tag 名本身。这会导致 GitHub Release 的正文与 annotated tag 的实际说明不一致，尤其是误用 lightweight tag 或空注释时，最终 Release 正文不再等于 tag 说明全文。

## 目标

- GitHub Release 的正文必须严格等于 annotated tag 的说明全文。
- 不再允许“读取不到说明时回退为 tag 名”。
- 如果推送的是 lightweight tag，发布流程必须直接失败。
- 如果 annotated tag 的说明为空，发布流程也必须直接失败。

## 方案

采用“先校验 tag 类型，再把 tag 说明全文作为 step output 直接传给 release action”的方案：

1. 在发布工作流中读取 `refs/tags/<tag>` 的 Git 对象类型。
2. 只有对象类型为 `tag` 时才继续执行，确保必须是 annotated tag。
3. 使用 Git 读取该 annotated tag 的完整说明全文。
4. 如果说明为空或全空白，直接让工作流失败。
5. 使用 GitHub Actions 的多行 step output 将正文直接传递给 `softprops/action-gh-release` 的 `body` 字段。
6. 移除中间文件 `release-notes.md` 和 tag 名 fallback 逻辑。

## 边界

- 轻量 tag 会在发布前失败，不会创建错误正文的 Release。
- 多行说明、空行、Markdown 列表等都应原样传入 Release 正文。
- Release 标题仍可继续使用 tag 名；本次只修复正文与 tag 说明不一致的问题。

## 受影响文件

- `.github/workflows/release.yml`：校验 annotated tag、读取说明全文、改为使用 `body` 直接传值。
- `README.md`：明确要求发版必须使用 annotated tag，且 Release 正文直接取其完整说明。

## 验证

- 本地构建不受影响。
- 检查 workflow YAML 语法和变量传递逻辑。
- 验证 README 中发版说明与新行为一致。
