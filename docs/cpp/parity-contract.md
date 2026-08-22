# Parity 契约

产品行为由版本化 golden、固定 GT 与 Native tests 共同约束。Python 曾用于生成迁移期基线，
但不再是在线 Oracle、产品运行时或可重新解释既有资产的权威。

> ADR-0038 已确认该目标。Python parity/benchmark 是隔离离线工具；产品验收以
> Native tests、版本化 golden 与 `scripts/verify-product.sh` 为准。

## 1. 行为真源

各类真源按职责共同约束产品，而非由某种实现语言决定：

| 真源 | 约束 |
|---|---|
| Native tests | Core、Pipeline、Ports、协议、Adapters 与产品入口的可执行合同 |
| 版本化 golden | 确定输入和配置下的中间决策与最终结果 |
| 固定 GT | timing、CER、usable、noise、empty 等产品质量水位 |
| 历史 Oracle metadata | 只证明既有 golden 的生成来源，不覆盖前三项 |

## 2. Golden provenance

既有正式 golden 必须保留：

| 字段 | 含义 |
|---|---|
| `oracle_commit` | 历史 Python 基线的精确 Git SHA；只作 provenance |
| 运行环境 | 生成资产时的完整环境；历史 Python 资产保留 Python、NumPy、OpenCV、FFmpeg，Vision 场景补充 OS/机型 |
| `input_asset_sha256` | 输入视频、帧或生成素材的指纹 |
| `config_fingerprint` | 完整 Config 的规范序列化哈希 |
| `golden_schema_version` | Golden 字段和比较规则版本 |

Native 报告同时记录 commit、编译器、构建类型、依赖指纹和 sanitizer 状态。禁止用未钉扎的
`main`、当前 Python 实现或其他活动代码重解释既有 golden。最终 Python 源码通过 Git
revision/tag 定位，不建立源码 archive。

## 3. 比较层级

| 层 | 对象 | 规则 |
|---|---|---|
| L0 | 枚举、整数、时间戳、dHash、事件、段边界、调用次数、代表帧 | exact |
| L1 | `fg_ratio`、SSIM 等浮点中间量 | schema 指定的 abs/rel epsilon |
| L2 | detection hash、段集合、Mock OCR 决策与 entries | exact |
| L3 | 固定 GT 的 timing、CER、usable、noise、empty | 不低于冻结水位 |
| L4 | Vision 等平台 OCR 文本 | 允许引擎波动；坐标、排序和结构仍按 L0/L3 |

最终 SRT 一致但中间决策不同仍视为 parity 失败，避免错误相互抵消。

## 4. Golden 内容

按模块保存足以定位差异的中间量：

- Config 与 Oracle metadata；
- Frame signature；
- changepoint events 与 timeline segments；
- OCR 调用、代表帧和行选择决策；
- OCR lines、去重前后 entries 与 detection hash；
- 最终 SRT 和质量指标。

资产、manifest 和小型 golden 位于 `benchmark/parity/`；生成与校验入口位于
`scripts/parity/`；C++ loader/comparator 位于 `cpp/src/test_support/` 和 `cpp/tests/`。

## 5. 数值与文本规则

- 所有 `*_ms`、hash、枚举、box 和 Mock 文本默认 exact。
- L1 浮点使用 schema 声明的 epsilon；新增字段默认 exact。
- 既有 golden 使用其 schema 冻结的 rounding；历史 Python 资产采用 bankers rounding，
  不得在无行为变更决策时改成 `std::round`。
- 浮点 JSON 使用足够精度且不依赖 locale。
- 文本按 golden schema 的 Unicode 规则比较；改变 normalization/strip 行为必须更新 schema。
- 时间戳、色域和坐标转换必须复现对应 golden 的已冻结语义，不得在迁移或优化中顺带修正。

## 6. 更新规则

只有有意接受行为变化时才能更新 golden 或 GT：

1. 明确变更原因和影响层级；
2. 更新输入/Config、Native producer 与依赖指纹；历史 `oracle_commit` 不伪造、不覆盖；
3. 必要时提升 `golden_schema_version`；
4. 用受控工具重生成 golden，并由 Native tests 复跑；
5. 同时验证中间量、最终输出与适用的 GT/runtime 门，并完成明确审查。

禁止为让失败测试转绿而单独覆盖 golden、放宽 epsilon 或跳过缺失资产。

## 7. 执行边界

- 日常离线测试读取已提交 golden，不启动 Python。
- Live Paddle/Vision、性能、长流和 GT 门仅在所需模型、媒体与依赖齐备时运行；缺失条件不得伪装为完整验收。
- `--skip-runtime`、`--skip-gt` 等降级模式只验证其实际执行部分。
- 产品 target 不依赖 test support 或 benchmark。
- Native-only 产品门必须能在不存在 Python、`.venv` 和 `PYTHONPATH` 的环境执行。
- 可选 Python 工具只能离线生成候选数据；是否接受结果由 golden review、GT 和 Native tests
  决定，工具本身不是行为真源。

历史 golden 生成、cutover 重放命令与 manifest 见
[`scripts/parity/README.md`](../../scripts/parity/README.md)；该工具入口不属于 Native 产品门。
