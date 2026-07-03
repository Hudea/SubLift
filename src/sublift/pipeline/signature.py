"""字幕帧签名计算。

计算两个独立信号：
- 信号 A：前景像素占比 → 检测字幕「存在 / 消失」边界
- 信号 B：dHash（差分哈希）→ 检测字幕「内容变化」边界

处理管线：
  字幕带裁剪 → 灰度化 → 自适应二值化 → 形态学过滤
                                    ↓
                            前景像素占比（信号 A）
                                    ↓
                          缩放到 (hash_size+1)×hash_size → dHash（信号 B）
"""

from __future__ import annotations

from dataclasses import dataclass

import cv2
import numpy as np

from sublift.config import SignatureConfig


@dataclass
class FrameSignature:
    """帧签名数据。

    Attributes:
        timestamp_ms: 帧时间戳（毫秒）。
        foreground_ratio: 前景像素占比（0.0~1.0）。
        dhash: 差分哈希值（int）。
    """

    timestamp_ms: int
    foreground_ratio: float
    dhash: int


def compute_signature(
    image: np.ndarray,
    timestamp_ms: int,
    config: SignatureConfig | None = None,
) -> FrameSignature:
    """计算帧的完整签名。

    Args:
        image: 字幕区域图像（RGB 或灰度，numpy array）。
        timestamp_ms: 帧时间戳（毫秒）。
        config: 签名计算配置，None 用默认值。

    Returns:
        FrameSignature 对象，含前景占比与 dHash。
    """
    if config is None:
        config = SignatureConfig()

    gray = _to_gray(image)
    foreground_ratio, binary = _compute_foreground_ratio(gray, config)
    dhash = compute_dhash(binary, config.hash_size)

    return FrameSignature(
        timestamp_ms=timestamp_ms,
        foreground_ratio=foreground_ratio,
        dhash=dhash,
    )


def compute_dhash(image: np.ndarray, hash_size: int = 8) -> int:
    """计算图像的差分哈希（dHash）。

    通过比较相邻像素的亮度梯度生成哈希值，
    对整体亮度漂移免疫，对结构变化敏感。

    Args:
        image: 灰度或二值化图像。
        hash_size: 哈希尺寸，默认 8x8=64bit。

    Returns:
        哈希值（整数）。
    """
    resized = cv2.resize(
        image, (hash_size + 1, hash_size), interpolation=cv2.INTER_AREA
    )
    diff = resized[:, 1:] > resized[:, :-1]

    hash_value = 0
    for row in diff:
        for bit in row:
            hash_value = (hash_value << 1) | int(bit)
    return hash_value


def hamming_distance(hash1: int, hash2: int) -> int:
    """计算两个哈希值的汉明距离。"""
    return bin(hash1 ^ hash2).count("1")


def compute_ssim(
    img1: np.ndarray,
    img2: np.ndarray,
    window_size: int = 7,
) -> float:
    """计算两张图像的结构相似度（SSIM）。

    numpy 回退实现，不依赖 scikit-image。
    作为 dHash 的二级验证，过滤误报。

    Args:
        img1: 第一张图像（灰度）。
        img2: 第二张图像（灰度）。
        window_size: 滑动窗口大小，必须为奇数。

    Returns:
        SSIM 值（0.0~1.0），越接近 1 表示越相似。
    """
    g1 = _to_gray(img1).astype(np.float64)
    g2 = _to_gray(img2).astype(np.float64)

    if g1.shape != g2.shape:
        g2 = np.asarray(
            cv2.resize(g2, (g1.shape[1], g1.shape[0]), interpolation=cv2.INTER_AREA),
            dtype=np.float64,
        )

    c1 = (0.01 * 255) ** 2
    c2 = (0.03 * 255) ** 2

    mu1 = cv2.GaussianBlur(g1, (window_size, window_size), 0)
    mu2 = cv2.GaussianBlur(g2, (window_size, window_size), 0)
    mu1_sq = mu1**2
    mu2_sq = mu2**2
    mu1_mu2 = mu1 * mu2

    sigma1_sq = cv2.GaussianBlur(g1 * g1, (window_size, window_size), 0) - mu1_sq
    sigma2_sq = cv2.GaussianBlur(g2 * g2, (window_size, window_size), 0) - mu2_sq
    sigma12 = cv2.GaussianBlur(g1 * g2, (window_size, window_size), 0) - mu1_mu2

    ssim_map = ((2 * mu1_mu2 + c1) * (2 * sigma12 + c2)) / (
        (mu1_sq + mu2_sq + c1) * (sigma1_sq + sigma2_sq + c2)
    )
    return float(ssim_map.mean())


def _to_gray(image: np.ndarray) -> np.ndarray:
    """转灰度图。已是灰度则原样返回。"""
    if len(image.shape) == 3:
        return cv2.cvtColor(image, cv2.COLOR_RGB2GRAY)
    return image


def _compute_block_size(band_height: int, ratio: float) -> int:
    """根据图像高度和比例计算 block_size，确保为奇数且 >= 3。"""
    size = max(3, int(band_height * ratio))
    return size if size % 2 == 1 else size + 1


def _compute_foreground_ratio(
    gray: np.ndarray,
    config: SignatureConfig,
) -> tuple[float, np.ndarray]:
    """计算前景像素占比。

    自适应二值化（高斯加权）→ 形态学开运算去噪 → 算白像素占比。

    Args:
        gray: 灰度图像。
        config: 签名配置。

    Returns:
        (前景像素占比, 二值化图像)。
    """
    block_size = _compute_block_size(gray.shape[0], config.block_size_ratio)

    binary = cv2.adaptiveThreshold(
        gray,
        255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY_INV,
        block_size,
        config.adaptive_c,
    )

    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (2, 2))
    binary = cv2.morphologyEx(binary, cv2.MORPH_OPEN, kernel)

    ratio = float(binary.sum()) / (255 * binary.size)
    return ratio, binary
