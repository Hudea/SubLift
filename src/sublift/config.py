"""SubLift 默认配置。"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class SignatureConfig:
    """帧签名计算配置。

    影响自适应二值化与 dHash 计算。
    """

    block_size_ratio: float = 0.08
    """自适应二值化块大小比例（相对字幕带高度）。"""

    adaptive_c: int = 12
    """自适应阈值常数 C，从局部均值减去。"""

    hash_size: int = 8
    """dHash 尺寸（hash_size x hash_size bit，默认 8x8=64bit）。"""


@dataclass(frozen=True)
class ChangePointConfig:
    """变化点检测配置。

    控制状态机迟滞、阈值与可选 SSIM 验证。
    """

    presence_threshold: float = 0.01
    """前景占比阈值，> 此值判定有字幕。"""

    hysteresis_frames: int = 1
    """迟滞确认帧数，连续 N 帧满足条件才迁移状态。

    feat-033c：从 2 降为 1，降低短字幕被 5fps 迟滞窗口吃掉的概率。
    """

    change_threshold: int = 10
    """dHash 汉明距离阈值，> 此值判定字幕内容变化。"""

    enable_ssim_verify: bool = False
    """是否启用 SSIM 两级验证（MVP 默认关闭，接口就位）。"""

    ssim_threshold: float = 0.95
    """SSIM 相似度阈值，> 此值判定为相似（否决 dHash 变化候选）。"""

    ssim_window_size: int = 7
    """SSIM 滑动窗口大小，必须为奇数。"""

    enable_ssim_patrol: bool = True
    """是否启用 SSIM 巡逻（feat-031b）。

    在 STABLE 状态下主动比较当前帧与锚帧的前景结构，当 dHash 未触发
    但 SSIM 显示结构变化明显时生成 CHANGE 候选。默认开启（对比验证
    precision 不下降，F1 提升 +15.6pp）。
    """

    ssim_patrol_interval: int = 3
    """SSIM 巡逻间隔（帧数），每 N 帧执行一次巡逻。"""

    ssim_patrol_threshold: float = 0.92
    """SSIM 巡逻阈值，前景 SSIM < 此值视为字幕结构变化。"""

    ssim_patrol_use_mask: bool = True
    """巡逻时是否优先比较二值化前景 mask（而非 raw crop）。

    True 时对锚帧与当前帧分别做自适应二值化，在二值图上算 SSIM，
    能屏蔽背景画面变化，只看字幕前景结构差异。
    """


@dataclass(frozen=True)
class Config:
    """提取流程默认配置。"""

    sample_fps: float = 5.0
    region_bottom_ratio: float = 0.3
    confidence_threshold: float = 0.5
    merge_gap_ms: int = 1000
    min_duration_ms: int = 500
    ocr_anchor_delay_frames: int = 2
    """IN/CHANGE 后延迟 N 帧再锁定 OCR 锚帧（feat-033b）。

    事件触发帧常落在字幕淡入/切换过渡上，Vision 易返回空文本；
    延迟 2 帧（@5fps ≈400ms）更易落到稳定字幕画面。段在延迟内闭合时回退首帧。
    """
    drop_empty_text: bool = False
    """是否丢弃 OCR 空文本段（feat-033b）。

    False（默认）：保留时间轴命中，即使文本为空（评测 timing 不因 OCR
    失败变成 no_overlap；编辑器可见空行）。True：旧行为，strip 后为空则丢弃。
    """
    signature: SignatureConfig = SignatureConfig()
    change_point: ChangePointConfig = ChangePointConfig()


DEFAULT_CONFIG = Config()
