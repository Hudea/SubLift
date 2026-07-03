"""帧签名计算测试。

用 numpy/cv2 造合成图像，不依赖外部资源。
"""

from __future__ import annotations

import numpy as np

from sublift.config import SignatureConfig
from sublift.pipeline.signature import (
    compute_dhash,
    compute_signature,
    hamming_distance,
)


def _blank_image(width: int = 320, height: int = 80) -> np.ndarray:
    """造纯色空白图（无字幕，亮背景）。"""
    return np.full((height, width, 3), 200, dtype=np.uint8)


def _text_like_image(
    width: int = 320,
    height: int = 80,
    rect: tuple[int, int, int, int] | None = None,
) -> np.ndarray:
    """造带暗色矩形的亮背景图（模拟亮背景+暗字幕）。

    自适应二值化用 THRESH_BINARY_INV（暗像素→前景），
    故测试图像应为亮背景+暗字幕。尺寸接近真实字幕带，使 block_size 落到合理范围。

    Args:
        rect: (x, y, w, h) 矩形区域，None 用默认居中。
    """
    img = np.full((height, width, 3), 200, dtype=np.uint8)
    if rect is None:
        rect = (60, 30, 200, 40)
    x, y, w, h = rect
    img[y : y + h, x : x + w] = 30
    return img


class TestForegroundRatio:
    """信号 A：前景像素占比。"""

    def test_blank_image_low_ratio(self) -> None:
        """空白图前景占比应接近 0。"""
        sig = compute_signature(_blank_image(), 0)
        assert sig.foreground_ratio < 0.05

    def test_text_image_high_ratio(self) -> None:
        """带字幕图前景占比应 > 默认 presence_threshold (0.01)。"""
        sig = compute_signature(_text_like_image(), 0)
        assert sig.foreground_ratio > 0.01

    def test_ratio_increases_with_text_size(self) -> None:
        """字幕越大，前景占比越高。"""
        small = compute_signature(_text_like_image(rect=(120, 50, 80, 20)), 0)
        large = compute_signature(_text_like_image(rect=(20, 10, 280, 60)), 0)
        assert large.foreground_ratio > small.foreground_ratio


class TestDHash:
    """信号 B：差分哈希。"""

    def test_same_image_zero_distance(self) -> None:
        """相同图像汉明距离为 0。"""
        img = _text_like_image()
        sig1 = compute_signature(img, 0)
        sig2 = compute_signature(img, 0)
        assert hamming_distance(sig1.dhash, sig2.dhash) == 0

    def test_different_text_nonzero_distance(self) -> None:
        """不同字幕汉明距离 > 0。"""
        img_a = _text_like_image(rect=(20, 20, 100, 40))
        img_b = _text_like_image(rect=(200, 20, 100, 40))
        sig_a = compute_signature(img_a, 0)
        sig_b = compute_signature(img_b, 0)
        assert hamming_distance(sig_a.dhash, sig_b.dhash) > 0

    def test_dhash_stable_across_brightness(self) -> None:
        """dHash 对整体亮度漂移免疫（只对结构变化敏感）。"""
        img_dark = _text_like_image()
        img_bright = img_dark + 50
        sig_dark = compute_signature(img_dark, 0)
        sig_bright = compute_signature(img_bright, 0)
        assert hamming_distance(sig_dark.dhash, sig_bright.dhash) <= 5


class TestComputeSignature:
    """compute_signature 整体行为。"""

    def test_timestamp_recorded(self) -> None:
        sig = compute_signature(_blank_image(), 12345)
        assert sig.timestamp_ms == 12345

    def test_custom_config(self) -> None:
        """自定义配置应生效（不报错）。"""
        config = SignatureConfig(block_size_ratio=0.1, adaptive_c=8, hash_size=8)
        sig = compute_signature(_text_like_image(), 0, config)
        assert sig.foreground_ratio > 0
        assert sig.dhash >= 0


class TestDHashDirect:
    """compute_dhash 直接测试。"""

    def test_uniform_image_dhash(self) -> None:
        """均匀图像 dHash 应为 0（无梯度）。"""
        uniform = np.full((40, 40), 128, dtype=np.uint8)
        assert compute_dhash(uniform) == 0

    def test_gradient_image_nonzero_dhash(self) -> None:
        """有梯度的图像 dHash 应非 0。"""
        gradient = np.tile(np.arange(0, 40, dtype=np.uint8), (40, 1))
        assert compute_dhash(gradient) > 0
