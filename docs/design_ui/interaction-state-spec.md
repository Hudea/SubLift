# Workbench 交互状态规范

状态由 `WorkspaceModel` 与 focused Core models 提供，View 不用多组互相矛盾的 Boolean 重建业务状态。

## 1. Workspace 状态

| 状态 | 主内容 | 编辑权限 | Inspector 默认 |
|---|---|---|---|
| `empty` | Welcome | 无 | 隐藏 |
| `loading` | 工作区骨架 | 无 | Video |
| `ready` | Video + Transcript 空态/既有结果 | 无最终结果时不可编辑 | Video |
| `regionEditing` | Region Overlay | 仅区域选择 | Region |
| `starting` / `processing` / `finalizing` | 进度 + Live Transcript | 只读 | Extraction |
| `review` | 最终 Transcript + Timeline | 可编辑 | Subtitle 或 Video |
| `failed` / `cancelled` | 可恢复上下文 + 错误/停止身份 | 仅保留的最终结果可编辑 | Extraction 或 Video |

## 2. 状态转换

```text
empty ──open/drop──► loading ──metadata ready──► ready
ready/review ──edit region──► regionEditing ──done──► ready/review
ready/review ──extract──► starting ─► processing ─► finalizing ─► review
starting/processing/finalizing ──stop──► cancelled
loading/starting/processing/finalizing ──error──► failed
```

- 每个窗口至多一个活动提取 job。
- 新视频清理 region、transcript selection、搜索和旧错误，但不修改 Settings。
- job token 失效后，迟到 progress、push 或 final entries 不得更新 UI。
- final entries 到达前不开放字幕编辑，避免最终结果覆盖用户修改。
- Runtime 路由只由 `RuntimePolicy` 决定，View 不实现 fallback。

## 3. 命令矩阵

| 动作 | empty | ready | region | processing | review | failed/cancelled |
|---|---:|---:|---:|---:|---:|---:|
| Open | ✓ | ✓ | — | — | ✓ | ✓ |
| Edit Region | — | ✓ | Active | — | ✓ | ✓ |
| Extract | — | ✓ | — | Stop | Re-extract | Retry |
| Export SRT | — | — | — | — | ✓ | 有最终结果时 ✓ |
| Show Inspector | — | ✓ | ✓ | ✓ | ✓ | ✓ |
| Edit / Split / Merge | — | — | — | — | 按选择启用 | 有最终结果时按选择启用 |

Toolbar、菜单、上下文菜单和快捷键必须绑定同一派生值。

## 4. Transcript 与 Inspector

- Processing/Finalizing：允许选择、seek、搜索；禁止编辑、Split、Merge 和 Export。
- Review：最终 entries 是可编辑源；Split/Merge 后 selection、currentId、Timeline 和条数同步。
- 打开视频后默认 Video；进入区域编辑强制 Region。
- 开始提取切到 Extraction；完成后有选中字幕则 Subtitle，否则 Video。
- 自动切换 mode 不得强制打开已关闭的 Inspector。
- Settings 和 Quick Extraction Settings 共用偏好；运行中控件锁定但值保持可读。

## 5. 错误与恢复

| 层级 | 用途 |
|---|---|
| Inline | 局部 capability、模型或配置问题，界面仍可继续 |
| Sheet | 替换视频、覆盖输出等需要用户确认的动作 |
| Alert | 文件、ffmpeg 或系统依赖阻断当前操作 |

错误文案必须说明原因和下一安全动作，不承诺自动下载、换引擎或回退 Python。

## 6. 响应式与辅助功能

- 1280×800：Video、Transcript 与 Inspector 可同时使用。
- 960×600：Inspector 可收起；Transcript 保持可读，主 Toolbar 动作不裁切。
- 变窄时先回收 Inspector，再压缩 Transcript；视频始终可见。
- Light/Dark、Increase Contrast、Reduce Transparency 下，选择、进度和边界仍可辨。
- Toolbar、Region、Progress、Transcript、Timeline 与错误均提供可读 label/value；不能只靠颜色表达状态。

实际视觉参考见 [`evidence/`](evidence/)；截图用于回看设计与验证，不代替状态和行为测试。
