import { describe, it, expect } from 'vitest';
import fs from 'fs';
import path from 'path';
import { calculateVideoViewport, normalizedToCanvas, canvasToNormalized } from '../utils/coordinate_mapper';

describe('Feature 12516: 960×600 Compact Viewport & 200% Zoom Responsiveness Verification', () => {
  describe('1. WorkbenchView.vue 960×600 Layout & Independent Scrolling', () => {
    const filePath = path.resolve(__dirname, 'WorkbenchView.vue');
    const content = fs.readFileSync(filePath, 'utf-8');

    it('主布局容器具备双栏网格与自适应宽度，防止全局横向溢出', () => {
      expect(content).toContain('.sl-workbench-layout');
      expect(content).toContain('grid-template-columns: minmax(0, 1.4fr) minmax(320px, 1fr)');
      expect(content).toContain('overflow: hidden');
      expect(content).toContain('box-sizing: border-box');
    });

    it('左侧控制面板具备独立垂直滚动能力 (overflow-y: auto)', () => {
      expect(content).toContain('.sl-left-pane');
      expect(content).toContain('overflow-y: auto');
      expect(content).toContain('overflow-x: hidden');
      expect(content).toContain('min-height: 0');
    });

    it('右侧字幕面板具备独立高度约束与嵌套滚动', () => {
      expect(content).toContain('.sl-right-pane');
      expect(content).toContain('min-height: 0');
      expect(content).toContain('overflow: hidden');
    });

    it('视频舞台容器设置最大高度与最小高度，适配 600px 紧凑视窗', () => {
      expect(content).toContain('.sl-video-stage-container');
      expect(content).toContain('aspect-ratio: 16 / 9');
      expect(content).toContain('max-height: calc(100vh - 160px)');
      expect(content).toContain('min-height: 200px');
    });

    it('包含 max-width: 960px 响应式单栏纵向折叠规则', () => {
      expect(content).toContain('@media (max-width: 960px)');
      expect(content).toContain('grid-template-columns: 1fr');
      expect(content).toContain('grid-template-rows: auto 1fr');
    });
  });

  describe('2. TaskCenterView.vue 960×600 Compact Layout', () => {
    const filePath = path.resolve(__dirname, 'TaskCenterView.vue');
    const content = fs.readFileSync(filePath, 'utf-8');

    it('主容器限制宽度并支持独立垂直滚动，防止视口越界', () => {
      expect(content).toContain('.sl-task-center-layout');
      expect(content).toContain('max-width: 1360px');
      expect(content).toContain('overflow-y: auto');
      expect(content).toContain('overflow-x: hidden');
      expect(content).toContain('box-sizing: border-box');
    });

    it('表格容器具备独立横向滚动条 (overflow-x: auto)', () => {
      expect(content).toContain('.sl-table-container');
      expect(content).toContain('overflow-x: auto');
    });

    it('操作工具栏支持 flex-wrap 折叠换行', () => {
      expect(content).toContain('.sl-queue-toolbar');
      expect(content).toContain('flex-wrap: wrap');
    });

    it('包含 max-width: 960px 紧凑内边距响应规则', () => {
      expect(content).toContain('@media (max-width: 960px)');
      expect(content).toContain('padding: 12px 14px');
    });
  });

  describe('3. Navbar.vue Compact Layout & Ellipsis', () => {
    const filePath = path.resolve(__dirname, '../components/Navbar.vue');
    const content = fs.readFileSync(filePath, 'utf-8');

    it('顶栏支持在紧凑屏幕下隐藏溢出文字并保持 48px 固定高度', () => {
      expect(content).toContain('height: 48px');
      expect(content).toContain('text-overflow: ellipsis');
      expect(content).toContain('@media (max-width: 960px)');
      expect(content).toContain('max-width: 90px');
    });
  });

  describe('4. BatchStatsCards.vue Multi-Breakpoint Grid', () => {
    const filePath = path.resolve(__dirname, '../components/BatchStatsCards.vue');
    const content = fs.readFileSync(filePath, 'utf-8');

    it('具备 7 列宽幅、4 列中幅、2 列窄幅的多级断点网格', () => {
      expect(content).toContain('grid-template-columns: repeat(7, minmax(0, 1fr))');
      expect(content).toContain('@media (max-width: 1100px)');
      expect(content).toContain('grid-template-columns: repeat(4, minmax(0, 1fr))');
      expect(content).toContain('@media (max-width: 768px)');
      expect(content).toContain('grid-template-columns: repeat(2, minmax(0, 1fr))');
    });
  });

  describe('5. 200% Zoom & Viewport Coordinate Math Integrity', () => {
    it('在 960×600 紧凑容器内正确计算 16:9 视频视口与选框坐标转换', () => {
      const containerWidth = 560;
      const containerHeight = 315;
      const metadata = {
        videoWidth: 1920,
        videoHeight: 1080,
        duration: 60,
        aspectRatio: 16 / 9,
      };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      expect(vp.width).toBeCloseTo(560, 1);
      expect(vp.height).toBeCloseTo(315, 1);
      expect(vp.x).toBeCloseTo(0, 1);
      expect(vp.y).toBeCloseTo(0, 1);

      // 默认底部 30% 选区映射
      const normBox = { x: 0, y: 0.7, width: 1, height: 0.3 };
      const canvasBox = normalizedToCanvas(normBox, vp);
      expect(canvasBox.x).toBeCloseTo(0, 1);
      expect(canvasBox.y).toBeCloseTo(315 * 0.7, 1);
      expect(canvasBox.width).toBeCloseTo(560, 1);
      expect(canvasBox.height).toBeCloseTo(315 * 0.3, 1);

      // 逆变换还原
      const roundtrip = canvasToNormalized(canvasBox, vp);
      expect(roundtrip.x).toBeCloseTo(0, 2);
      expect(roundtrip.y).toBeCloseTo(0.7, 2);
      expect(roundtrip.width).toBeCloseTo(1, 2);
      expect(roundtrip.height).toBeCloseTo(0.3, 2);
    });

    it('在 200% 高缩放下（模拟 480×300 逻辑视口）维持视口居中与黑边适配', () => {
      const containerWidth = 480;
      const containerHeight = 300;
      const metadata = {
        videoWidth: 1920,
        videoHeight: 1080,
        duration: 60,
        aspectRatio: 16 / 9,
      };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      expect(vp.width).toBeCloseTo(480, 1);
      expect(vp.height).toBeCloseTo(270, 1);
      expect(vp.x).toBeCloseTo(0, 1);
      expect(vp.y).toBeCloseTo(15, 1); // (300 - 270) / 2
    });
  });
});
