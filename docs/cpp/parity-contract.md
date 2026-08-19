# Parity 契约

Parity 用冻结 Python Oracle、版本化 golden 与 GT 约束 Native Candidate。Python 不作为 C++ 单元测试的在线隐式依赖。

## 1. 冻结 Oracle

正式 golden 必须记录：

| 字段 | 含义 |
|---|---|
| `oracle_commit` | Python 参考实现的精确 Git SHA |
| 运行环境 | Python、NumPy、OpenCV、FFmpeg；Vision 场景补充 OS/机型 |
| `input_asset_sha256` | 输入视频、帧或生成素材的指纹 |
| `config_fingerprint` | 完整 Config 的规范序列化哈希 |
| `golden_schema_version` | Golden 字段和比较规则版本 |

Candidate 报告同时记录 commit、编译器、构建类型、依赖指纹和 sanitizer 状态。禁止用未钉扎的
`main` 重解释既有 golden。

## 2. 比较层级

| 层 | 对象 | 规则 |
|---|---|---|
| L0 | 枚举、整数、时间戳、dHash、事件、段边界、调用次数、代表帧 | exact |
| L1 | `fg_ratio`、SSIM 等浮点中间量 | schema 指定的 abs/rel epsilon |
| L2 | detection hash、段集合、Mock OCR 决策与 entries | exact |
| L3 | 固定 GT 的 timing、CER、usable、noise、empty | 不低于冻结水位 |
| L4 | Vision 等平台 OCR 文本 | 允许引擎波动；坐标、排序和结构仍按 L0/L3 |

最终 SRT 一致但中间决策不同仍视为 parity 失败，避免错误相互抵消。

## 3. Golden 内容

按模块保存足以定位差异的中间量：

- Config 与 Oracle metadata；
- Frame signature；
- changepoint events 与 timeline segments；
- OCR 调用、代表帧和行选择决策；
- OCR lines、去重前后 entries 与 detection hash；
- 最终 SRT 和质量指标。

资产、manifest 和小型 golden 位于 `benchmark/parity/`；生成与校验入口位于
`scripts/parity/`；C++ loader/comparator 位于 `cpp/src/test_support/` 和 `cpp/tests/`。

## 4. 数值与文本规则

- 所有 `*_ms`、hash、枚举、box 和 Mock 文本默认 exact。
- L1 浮点使用 schema 声明的 epsilon；新增字段默认 exact。
- Parity 路径使用与 Python 一致的 bankers rounding，不以 `std::round` 替代。
- 浮点 JSON 使用足够精度且不依赖 locale。
- 文本按冻结 Oracle 的 Unicode 规则比较；改变 normalization/strip 行为必须更新 schema。
- 时间戳、色域和坐标转换必须复现对应 golden 的已冻结语义，不得在迁移或优化中顺带修正。

## 5. 更新规则

只有有意接受行为变化时才能更新 Oracle 或 golden：

1. 明确变更原因和影响层级；
2. 更新 `oracle_commit`、输入/Config 指纹；
3. 必要时提升 `golden_schema_version`；
4. 重生成 golden，并由 C++ Candidate 复跑；
5. 同时验证中间量、最终输出与适用的 GT/runtime 门。

禁止为让失败测试转绿而单独覆盖 golden、放宽 epsilon 或跳过缺失资产。

## 6. 执行边界

- 日常离线测试读取已提交 golden，不启动 Python。
- Live Paddle/Vision、性能、长流和 GT 门仅在所需模型、媒体与依赖齐备时运行；缺失条件不得伪装为完整验收。
- `--skip-runtime`、`--skip-gt` 等降级模式只验证其实际执行部分。
- 产品 target 不依赖 test support、diagnostics 或 benchmark。

具体命令、manifest 与各引擎门见 [`scripts/parity/README.md`](../../scripts/parity/README.md)。
