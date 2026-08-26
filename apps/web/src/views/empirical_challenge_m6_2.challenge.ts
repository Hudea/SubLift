import { describe, it, expect } from 'vitest';
import fs from 'fs';
import path from 'path';
import { calculateVideoViewport, normalizedToCanvas, canvasToNormalized } from '../utils/coordinate_mapper';

describe('Empirical Challenge M6-2: Viewport Responsiveness, 200% Zoom & Text Selectability', () => {
  const mainCss = fs.readFileSync(path.resolve(__dirname, '../styles/main.css'), 'utf-8');
  const workbenchView = fs.readFileSync(path.resolve(__dirname, 'WorkbenchView.vue'), 'utf-8');
  const taskCenterView = fs.readFileSync(path.resolve(__dirname, 'TaskCenterView.vue'), 'utf-8');
  const navbarComp = fs.readFileSync(path.resolve(__dirname, '../components/Navbar.vue'), 'utf-8');
  const transcriptComp = fs.readFileSync(path.resolve(__dirname, '../components/LiveTranscript.vue'), 'utf-8');
  const videoPlayerComp = fs.readFileSync(path.resolve(__dirname, '../components/VideoPlayer.vue'), 'utf-8');
  const inspectorComp = fs.readFileSync(path.resolve(__dirname, '../components/BatchTaskInspector.vue'), 'utf-8');
  const batchStatsComp = fs.readFileSync(path.resolve(__dirname, '../components/BatchStatsCards.vue'), 'utf-8');

  describe('1. Root Container & Viewport Responsiveness (960×600, 1024×768, 1280×800, 200% Zoom)', () => {
    it('Root container (html, body, #app) has overflow: hidden and width: 100% preventing horizontal scrollbar leaks', () => {
      const htmlBodyBlock = mainCss.match(/html,\s*body\s*\{([^}]+)\}/);
      expect(htmlBodyBlock).not.toBeNull();
      expect(htmlBodyBlock![1]).toContain('width: 100%');
      expect(htmlBodyBlock![1]).toContain('height: 100%');
      expect(htmlBodyBlock![1]).toContain('overflow: hidden');

      const appBlock = mainCss.match(/#app\s*\{([^}]+)\}/);
      expect(appBlock).not.toBeNull();
      expect(appBlock![1]).toContain('width: 100%');
      expect(appBlock![1]).toContain('height: 100%');
    });

    it('Workbench layout at 960×600 utilizes CSS Grid minmax bounds with zero horizontal overflow', () => {
      expect(workbenchView).toContain('grid-template-columns: minmax(0, 1.4fr) minmax(320px, 1fr)');
      expect(workbenchView).toContain('height: calc(100vh - 48px)');
      expect(workbenchView).toContain('overflow: hidden');
      expect(workbenchView).toContain('box-sizing: border-box');
    });

    it('Workbench layout automatically folds into responsive single-column at width <= 960px for 200% zoom', () => {
      expect(workbenchView).toContain('@media (max-width: 960px)');
      expect(workbenchView).toContain('grid-template-columns: 1fr');
      expect(workbenchView).toContain('grid-template-rows: auto 1fr');
      expect(workbenchView).toContain('overflow-y: auto');
    });

    it('Task center layout uses bounded max-width (1360px) with centered alignment and vertical scroll', () => {
      expect(taskCenterView).toContain('max-width: 1360px');
      expect(taskCenterView).toContain('margin: 0 auto');
      expect(taskCenterView).toContain('overflow-y: auto');
      expect(taskCenterView).toContain('overflow-x: hidden');
    });

    it('TaskCenter table container isolates table horizontal scrolling to prevent root page horizontal scrolling', () => {
      expect(taskCenterView).toContain('.sl-table-container');
      expect(taskCenterView).toContain('overflow-x: auto');
    });

    it('Navbar adapts to 960px and 200% zoom with bounded text truncation (max-width: 90px on mobile/compact)', () => {
      expect(navbarComp).toContain('@media (max-width: 960px)');
      expect(navbarComp).toContain('max-width: 90px');
      expect(navbarComp).toContain('overflow: hidden');
      expect(navbarComp).toContain('text-overflow: ellipsis');
    });

    it('BatchStatsCards implements responsive multi-tier breakpoints (7 cols -> 4 cols @1100px -> 2 cols @768px)', () => {
      expect(batchStatsComp).toContain('grid-template-columns: repeat(7, minmax(0, 1fr))');
      expect(batchStatsComp).toContain('@media (max-width: 1100px)');
      expect(batchStatsComp).toContain('grid-template-columns: repeat(4, minmax(0, 1fr))');
      expect(batchStatsComp).toContain('@media (max-width: 768px)');
      expect(batchStatsComp).toContain('grid-template-columns: repeat(2, minmax(0, 1fr))');
    });
  });

  describe('2. Independent Scrolling for Panes, Tables & Transcripts', () => {
    it('Workbench left pane scrolls independently (overflow-y: auto, min-height: 0)', () => {
      expect(workbenchView).toContain('.sl-left-pane');
      expect(workbenchView).toContain('overflow-y: auto');
      expect(workbenchView).toContain('min-height: 0');
    });

    it('Workbench right pane constrains child heights (height: 100%, min-height: 0, overflow: hidden)', () => {
      expect(workbenchView).toContain('.sl-right-pane');
      expect(workbenchView).toContain('height: 100%');
      expect(workbenchView).toContain('min-height: 0');
      expect(workbenchView).toContain('overflow: hidden');
    });

    it('LiveTranscript body container has independent vertical scrolling with tabindex for keyboard navigation', () => {
      expect(transcriptComp).toContain('.sl-transcript-body');
      expect(transcriptComp).toContain('overflow-y: auto');
      expect(transcriptComp).toContain('min-height: 0');
      expect(transcriptComp).toContain('tabindex="0"');
      expect(transcriptComp).toContain('aria-label="字幕条目滚动列表"');
    });

    it('BatchTaskInspector wraps long file paths and callouts to avoid forcing horizontal scrollbar', () => {
      expect(inspectorComp).toContain('.sl-ins-path');
      expect(inspectorComp).toContain('text-overflow: ellipsis');
      expect(inspectorComp).toContain('.sl-warning-content');
      expect(inspectorComp).toContain('word-break: break-all');
      expect(inspectorComp).toContain('.sl-error-content');
    });
  });

  describe('3. Toolbar Visibility, Clickability & Non-Overlapping Layouts', () => {
    it('Workbench top bar uses flex-wrap and gap spacing to prevent button overlapping', () => {
      expect(workbenchView).toContain('.sl-workbench-top-bar');
      expect(workbenchView).toContain('display: flex');
      expect(workbenchView).toContain('justify-content: space-between');
      expect(workbenchView).toContain('flex-wrap: wrap');
      expect(workbenchView).toContain('gap: 10px');
    });

    it('Workbench quick aux toolbar supports multi-button wrapping without clipping', () => {
      expect(workbenchView).toContain('.sl-quick-aux-bar');
      expect(workbenchView).toContain('display: flex');
      expect(workbenchView).toContain('flex-wrap: wrap');
      expect(workbenchView).toContain('gap: 8px');
    });

    it('LiveTranscript toolbar header provides wrap and spacing for undo/redo, search and export buttons', () => {
      expect(transcriptComp).toContain('.sl-card-header');
      expect(transcriptComp).toContain('display: flex');
      expect(transcriptComp).toContain('flex-wrap: wrap');
      expect(transcriptComp).toContain('gap: 8px');
    });

    it('VideoPlayer control bar maintains pill layout with min-width scrubber and non-overlapping controls', () => {
      expect(videoPlayerComp).toContain('.sl-player-bar');
      expect(videoPlayerComp).toContain('display: flex');
      expect(videoPlayerComp).toContain('gap: 8px');
      expect(videoPlayerComp).toContain('.sl-scrubber');
      expect(videoPlayerComp).toContain('min-width: 60px');
      expect(videoPlayerComp).toContain('flex: 1');
    });

    it('TaskCenter queue toolbar provides flex-wrap with separated left and right clusters', () => {
      expect(taskCenterView).toContain('.sl-queue-toolbar');
      expect(taskCenterView).toContain('display: flex');
      expect(taskCenterView).toContain('flex-wrap: wrap');
      expect(taskCenterView).toContain('gap: 12px');
      expect(taskCenterView).toContain('.sl-toolbar-left');
      expect(taskCenterView).toContain('.sl-toolbar-right');
    });

    it('Inspector action toolbar supports flex-wrap for multi-action button rows', () => {
      expect(inspectorComp).toContain('.sl-inspector-actions');
      expect(inspectorComp).toContain('display: flex');
      expect(inspectorComp).toContain('flex-wrap: wrap');
      expect(inspectorComp).toContain('gap: 5px');
    });
  });

  describe('4. Text Selectability across All Core Elements', () => {
    it('Global html, body does NOT enforce user-select: none', () => {
      const htmlBodyBlock = mainCss.match(/html,\s*body\s*\{([^}]+)\}/);
      expect(htmlBodyBlock![1]).not.toContain('user-select: none');
      expect(htmlBodyBlock![1]).not.toContain('-webkit-user-select: none');
    });

    it('main.css explicitly enables user-select: text on subtitle rows, timestamps, file paths, logs and errors', () => {
      const selectableSelectors = [
        '.sl-row-text',
        '.sl-time-cell',
        '.sl-timecode',
        '.sl-file-title',
        '.sl-file-subpath',
        '.sl-ins-path',
        '.sl-meta-chip',
        '.sl-status-banner-text',
        '.sl-warning-content',
        '.sl-error-content',
        '.sl-ws-text',
        '.sl-summary-text',
      ];

      for (const selector of selectableSelectors) {
        expect(mainCss).toContain(selector);
      }
      expect(mainCss).toContain('user-select: text');
      expect(mainCss).toContain('-webkit-user-select: text');
    });

    it('Transcript row subtitle text and time cells have explicit user-select: text in LiveTranscript scoped CSS', () => {
      expect(transcriptComp).toContain('.sl-row-text');
      expect(transcriptComp).toContain('user-select: text');
      expect(transcriptComp).toContain('.sl-time-cell');
      expect(transcriptComp).toContain('user-select: text');
    });

    it('VideoPlayer timecode has explicit user-select: text', () => {
      expect(videoPlayerComp).toContain('.sl-timecode');
      expect(videoPlayerComp).toContain('user-select: text');
    });

    it('Inspector file path, tags, chips and callout content have explicit user-select: text', () => {
      expect(inspectorComp).toContain('.sl-inspector-filename');
      expect(inspectorComp).toContain('user-select: text');
      expect(inspectorComp).toContain('.sl-ins-path');
      expect(inspectorComp).toContain('user-select: text');
      expect(inspectorComp).toContain('.sl-meta-chip');
      expect(inspectorComp).toContain('user-select: text');
      expect(inspectorComp).toContain('.sl-warning-content');
      expect(inspectorComp).toContain('user-select: text');
      expect(inspectorComp).toContain('.sl-error-content');
      expect(inspectorComp).toContain('user-select: text');
    });
  });

  describe('5. Coordinate Mapper and Scaling Math at 200% Zoom on Multiple Ratios', () => {
    it('Accurately calculates viewport on 960×600 (aspect 16:10) for 16:9 video', () => {
      const containerWidth = 960;
      const containerHeight = 600;
      const metadata = { videoWidth: 1920, videoHeight: 1080, duration: 120, aspectRatio: 16 / 9 };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      // 960 / (16/9) = 540 <= 600 -> letterboxed top and bottom
      expect(vp.width).toBe(960);
      expect(vp.height).toBe(540);
      expect(vp.x).toBe(0);
      expect(vp.y).toBe((600 - 540) / 2); // 30px pillar/letterbox offset
    });

    it('Accurately calculates viewport on 1024×768 (aspect 4:3) for 16:9 video', () => {
      const containerWidth = 1024;
      const containerHeight = 768;
      const metadata = { videoWidth: 1920, videoHeight: 1080, duration: 120, aspectRatio: 16 / 9 };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      // 1024 / (16/9) = 576 <= 768
      expect(vp.width).toBe(1024);
      expect(vp.height).toBe(576);
      expect(vp.x).toBe(0);
      expect(vp.y).toBe((768 - 576) / 2); // 96px offset
    });

    it('Accurately calculates viewport on 1280×800 (aspect 16:10) for 16:9 video', () => {
      const containerWidth = 1280;
      const containerHeight = 800;
      const metadata = { videoWidth: 1920, videoHeight: 1080, duration: 120, aspectRatio: 16 / 9 };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      expect(vp.width).toBe(1280);
      expect(vp.height).toBe(720);
      expect(vp.x).toBe(0);
      expect(vp.y).toBe((800 - 720) / 2); // 40px offset
    });

    it('Maintains coordinate transformation precision on 200% zoom (480×300 simulated logical viewport)', () => {
      const containerWidth = 480;
      const containerHeight = 300;
      const metadata = { videoWidth: 3840, videoHeight: 2160, duration: 300, aspectRatio: 16 / 9 };

      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);
      expect(vp.width).toBe(480);
      expect(vp.height).toBe(270);
      expect(vp.x).toBe(0);
      expect(vp.y).toBe(15);

      // Custom ROI box in normalized coords [0.1, 0.65, 0.8, 0.25]
      const inputNorm = { x: 0.1, y: 0.65, width: 0.8, height: 0.25 };
      const canvasBox = normalizedToCanvas(inputNorm, vp);

      expect(canvasBox.x).toBeCloseTo(48, 1);
      expect(canvasBox.y).toBeCloseTo(15 + 270 * 0.65, 1);
      expect(canvasBox.width).toBeCloseTo(480 * 0.8, 1);
      expect(canvasBox.height).toBeCloseTo(270 * 0.25, 1);

      const recoveredNorm = canvasToNormalized(canvasBox, vp);
      expect(recoveredNorm.x).toBeCloseTo(inputNorm.x, 3);
      expect(recoveredNorm.y).toBeCloseTo(inputNorm.y, 3);
      expect(recoveredNorm.width).toBeCloseTo(inputNorm.width, 3);
      expect(recoveredNorm.height).toBeCloseTo(inputNorm.height, 3);
    });

    it('Clamps bounding box accurately when coordinates reach viewport boundaries', () => {
      const containerWidth = 480;
      const containerHeight = 300;
      const metadata = { videoWidth: 1920, videoHeight: 1080, duration: 100, aspectRatio: 16 / 9 };
      const vp = calculateVideoViewport(containerWidth, containerHeight, metadata);

      // Box exceeding right and bottom boundaries
      const oversizedCanvas = { x: -20, y: 5, width: 600, height: 350 };
      const norm = canvasToNormalized(oversizedCanvas, vp);

      expect(norm.x).toBe(0); // clamped to 0
      expect(norm.y).toBe(0); // clamped to 0
      expect(norm.width).toBeLessThanOrEqual(1.0);
      expect(norm.height).toBeLessThanOrEqual(1.0);
    });
  });

  describe('6. Motion & Focus Visual Affordances', () => {
    it('Universal :focus-visible outlines are defined with 2px accent outline and offset', () => {
      expect(mainCss).toContain(':focus-visible');
      expect(mainCss).toContain('outline: 2px solid var(--sl-color-accent)');
      expect(mainCss).toContain('outline-offset: 2px');
    });

    it('prefers-reduced-motion media query suppresses all animation durations and infinite spinners', () => {
      expect(mainCss).toContain('@media (prefers-reduced-motion: reduce)');
      expect(mainCss).toContain('animation-duration: 0.01ms !important');
      expect(mainCss).toContain('animation-iteration-count: 1 !important');
      expect(mainCss).toContain('transition-duration: 0.01ms !important');
      expect(mainCss).toContain('.sl-loading-spinner');
      expect(mainCss).toContain('.sl-btn-spinner');
      expect(mainCss).toContain('.sl-aux-spinner');
      expect(mainCss).toContain('.sl-beacon-light');
      expect(mainCss).toContain('.sl-live-dot');
    });
  });
});
