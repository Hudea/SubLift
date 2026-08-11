# 交互状态与视觉验收规范

本文件把主设计方案转成可测试的状态、动作和截图合同。测试用状态必须来自 `WorkspaceModel` 与现有 Core 对象，不能在 View 内用多组互相矛盾的 Boolean 拼接。

## 1. Workspace 状态

| 状态 | 可见主内容 | 主动作 | 编辑权限 | Inspector 默认模式 |
|---|---|---|---|---|
| empty | Welcome | Open | 无 | 隐藏 |
| dragActive | Drop target feedback | 接收/拒绝拖入 | 无 | 保持原状态 |
| loading | 工作区骨架/确定性进度 | 禁用重复导入或允许取消加载 | 无 | Video |
| ready | Video + 空/既有 Transcript | Extract | 无结果时不可编辑 | Video |
| regionEditing | 全部候选与 merged region | Done | 只允许区域选择 | Region |
| starting | 工作区保持 | Stop | Live Transcript 只读 | Extraction |
| processing | Video + 流式 Transcript + 真实进度 | Stop | 只读，可选择/seek/search | Extraction |
| finalizing | Video + 流式 Transcript + finalizing 状态 | Stop（若后端仍支持） | 只读 | Extraction |
| review | Video + 最终 Transcript | Export | 文本/拆分/合并可用 | Subtitle（有选择时） |
| failed | 保留可恢复上下文 + inline/alert 错误 | Retry/Extract | 保留最后一个已完成结果 | Extraction 或 Video |
| cancelled | 工作区保持、明确已停止 | Extract Again | 不把流式临时结果当最终结果 | Extraction |

## 2. 核心转换

```text
empty --open/drop--> loading --metadata ready--> ready
ready --edit region--> regionEditing --done--> ready
ready/review --extract--> starting --> processing --> finalizing --> review
starting/processing/finalizing --stop--> cancelled --> ready
loading/starting/processing/finalizing --error--> failed --retry--> prior safe state
review --new video--> loading
```

不变量：

- 每个 Window 只有一个活动 `WorkspaceSession` 和至多一个活动提取 job。
- 新视频载入会清理 region、transcript selection、临时搜索与旧错误，但不会修改 Settings。
- job token 失效后，迟到的 progress/push/final entries 不得更新 UI。
- processing/finalizing 的增量条目与 review 的最终条目是不同可编辑性阶段。
- final entries 替换增量列表时，不能覆盖任何已允许的用户编辑；因此在替换前不开放编辑。
- 默认 C++ 与显式 Python 的路由只由 `RuntimePolicy` 决定，View 不实现 fallback。

## 3. Toolbar 与命令矩阵

| 动作 | empty | ready | region | processing | review | failed/cancelled |
|---|---:|---:|---:|---:|---:|---:|
| Open | ✓ | ✓ | — | — | ✓ | ✓ |
| Edit Region | — | ✓ | Active | — | ✓ | ✓ |
| Extract | — | ✓ | — | Stop | Re-extract | Retry |
| Export SRT | — | — | — | — | ✓ | 仅有最终结果时 ✓ |
| Show Inspector | — | ✓ | ✓ | ✓ | ✓ | ✓ |
| Edit/Split/Merge | — | — | — | — | ✓ | 仅有最终结果时 ✓ |

菜单、Toolbar、上下文菜单和键盘快捷键必须绑定同一 command availability；视觉禁用与 action guard 同时存在。

## 4. Transcript 状态合同

### Ready / Empty

- 显示“尚未提取字幕”的次级空状态；不把导出、拆分、合并作为可用按钮。

### Processing / Finalizing

- 流式条目按时间排序出现；可选择、seek、搜索。
- 文本编辑器、Split、Merge、Export 均不可用；VoiceOver 说明“识别结果生成中，只读”。
- 当前播放高亮、用户选择和搜索命中是三个独立状态。

### Review

- 最终条目一次性切换为可编辑源；保留正常选择/seek。
- 低置信警告阈值使用既有产品配置或显式 UI 常量并有测试，不在 View 中散落 magic number。
- Split/Merge 后 selection/currentId 合法，Timeline 与条数同步更新。

## 5. Inspector 切换规则

- 打开视频后默认 Video。
- 进入区域编辑强制 Region；退出后恢复进入前模式或 Video。
- 开始提取切到 Extraction；用户可关闭 Inspector，但重新打开时仍是 Extraction。
- 完成后，有选中字幕则 Subtitle，无选择则 Video。
- 选择字幕时可自动切到 Subtitle，但不得强制打开已被用户关闭的 Inspector。
- 设置是独立 Window，不占用 Workspace Inspector。

## 6. 错误层级

| 层级 | 使用条件 | 例子 | 恢复要求 |
|---|---|---|---|
| Inline | 局部能力不可用但界面可继续 | Paddle capability/model 缺失 | 明确原因和下一安全动作 |
| Sheet | 需要用户确认的 Session 操作 | 新视频将替换未导出的编辑 | Cancel 为安全默认 |
| Alert | 当前动作被系统依赖阻断 | MKV 无 ffmpeg、文件无法打开 | 技术命令进入详情，不占主正文 |

错误文案不得承诺静默回退 Python、切换其他引擎或自动下载安装。

## 7. 响应式合同

| 视口 | 预期 |
|---|---|
| 1280 × 800 | Video、Transcript、Inspector 可同时使用；无裁切与重叠 |
| 960 × 600 | Inspector 默认收起或可收起；Transcript ≥320 pt；Toolbar 主动作完整 |
| 从宽到窄拖动 | 先回收 Inspector，再压缩 Transcript；视频保持可见；Split divider 可恢复 |
| 全屏/更宽 | 内容比例受约束，不把 Inspector 或 Transcript 无上限拉宽 |

Phase 10 不再支持把全部功能塞进 800 × 480；SwiftUI window minimum 应与 960 × 600 合同一致。

## 8. 视觉与辅助功能证据矩阵

每个实现 Feature 的 evidence 应列出具体测试与截图路径。最终收口至少包含：

| 证据 ID | 状态/环境 | 必须证明 |
|---|---|---|
| V01 | empty，1280×800，Light | Welcome 层级、Open 主动作、无 Dashboard Card |
| V02 | dragActive，Light | 合法拖入的边界、文字和非纯颜色反馈 |
| V03 | ready + Video Inspector，1280×800 | 60/40 内容比例、Toolbar、真实 metadata |
| V04 | regionEditing，1280×800 | 候选/选中/merged 语义一致，高级几何折叠 |
| V05 | processing，1280×800 | 真实进度/runtime、Stop、Live Transcript 只读 |
| V06 | review + Subtitle Inspector，1280×800 | 可编辑切换、confidence、Export 主动作 |
| V07 | Settings Recognition，Light | Vision/Paddle 状态与 Mock 隔离 |
| V08 | ready，960×600 | Inspector 收起后的最小布局无裁切 |
| V09 | ready/review，Dark | 系统语义色、视频黑底和选择状态可辨 |
| V10 | Increase Contrast + Reduce Transparency | 轮廓、选择、进度和浮层仍可辨 |
| A01 | 键盘 | Open、Space、Inspector、Region、字幕前后定位、Export 可达 |
| A02 | VoiceOver | Toolbar、Region、Progress、Transcript、Timeline 有名称和值 |

截图必须来自实际运行的 SwiftUI UI，使用稳定 fixture/mock 数据；设计参考图不能替代实现截图。

## 9. Feature 级验证最低要求

- 纯模型/状态：Swift XCTest 覆盖成功、失败、取消、迟到回调和非法转换。
- 纯布局计算：确定性单测覆盖宽/窄窗口和边界数值。
- View 行为：可测试的 accessibility identifier、command enabled state 和 snapshot/screenshot fixture。
- 运行时边界：既有 `RuntimePolicyTests`、`PipelineClientTests`、`SubtitleExtractorLogTests` 不回归。
- 每个 Feature 完成时运行 `cd apps/macos && swift test`，Phase 收口再运行项目标准验证。
