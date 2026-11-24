import { describe, expect, it } from 'vitest';
import { selectWorkbenchCommands, type WorkbenchCommandsInput } from './commands';
import { buildMediaUrl } from './mediaUrl';

function baseInput(
  overrides?: Partial<WorkbenchCommandsInput>
): WorkbenchCommandsInput {
  return {
    hasActiveTask: true,
    activeStatus: 'ready',
    activeProbe: {
      width: 1920,
      height: 1080,
      duration_ms: 60_000,
      fps: 24,
      display_transform_ok: true,
      transform_note: null,
    },
    activeEngine: 'paddle',
    capabilities: {
      engines: ['paddle'],
      ffmpeg_available: true,
      hwaccel_modes: [],
    },
    hasValidVlmProvider: true,
    isAutoDetecting: false,
    isReRecognizing: false,
    isDirtyInlineEdit: false,
    isInlineTextFocused: false,
    activeCueCount: 10,
    queueLength: 2,
    hasApplyTargets: true,
    exportableQueueCount: 1,
    canUndo: true,
    canRedo: false,
    ...overrides,
  };
}

describe('core/commands & core/mediaUrl', () => {
  it('blocks ROI and extraction when display_transform_ok is false', () => {
    const cmds = selectWorkbenchCommands(
      baseInput({
        activeStatus: 'idle',
        activeProbe: {
          width: 1920,
          height: 1080,
          duration_ms: 60_000,
          fps: 24,
          display_transform_ok: false,
          transform_note: '异常矩阵变换',
        },
      })
    );
    expect(cmds.detectAutoRegion.enabled).toBe(false);
    expect(cmds.editRoi.enabled).toBe(false);
    expect(cmds.startExtraction.enabled).toBe(false);
    expect(cmds.startExtraction.reason).toContain('异常矩阵变换');
  });

  it('blocks startExtraction when restored apple_vision draft is unsupported', () => {
    const cmds = selectWorkbenchCommands(
      baseInput({
        activeStatus: 'idle',
        activeEngine: 'apple_vision',
        capabilities: {
          engines: ['paddle'],
          ffmpeg_available: true,
          hwaccel_modes: [],
        },
      })
    );
    expect(cmds.startExtraction.enabled).toBe(false);
    expect(cmds.startExtraction.reason).toContain('Apple Vision');
  });

  it('gates edit, export, re-recognize, and undo when dirty inline edit is active', () => {
    const cmds = selectWorkbenchCommands(
      baseInput({
        isDirtyInlineEdit: true,
        isInlineTextFocused: true,
      })
    );
    expect(cmds.exportActiveSrt.enabled).toBe(false);
    expect(cmds.batchExportSrt.enabled).toBe(false);
    expect(cmds.reRecognizeLowConf.enabled).toBe(false);
    expect(cmds.undo.enabled).toBe(false);
    expect(cmds.redo.enabled).toBe(false);
  });

  it('locks cue editing and ROI during extracting while keeping addVideos & cancel enabled', () => {
    const cmds = selectWorkbenchCommands(
      baseInput({
        activeStatus: 'extracting',
      })
    );
    expect(cmds.addVideos.enabled).toBe(true);
    expect(cmds.cancelExtraction.enabled).toBe(true);
    expect(cmds.editRoi.enabled).toBe(false);
    expect(cmds.editCue.enabled).toBe(false);
    expect(cmds.exportActiveSrt.enabled).toBe(false);
  });

  it('blocks startExtraction, cue edits, undo/redo, and reRecognizeLowConf while isReRecognizing is active', () => {
    const cmds = selectWorkbenchCommands(
      baseInput({
        isReRecognizing: true,
        canUndo: true,
        canRedo: true,
      })
    );
    expect(cmds.startExtraction.enabled).toBe(false);
    expect(cmds.editCue.enabled).toBe(false);
    expect(cmds.splitCue.enabled).toBe(false);
    expect(cmds.mergeCue.enabled).toBe(false);
    expect(cmds.reRecognizeLowConf.enabled).toBe(false);
    expect(cmds.undo.enabled).toBe(false);
    expect(cmds.redo.enabled).toBe(false);
  });

  it('gates startExtraction during isAutoDetecting and derives correct matrix across all 6 TaskStatus states', () => {
    const detectingCmds = selectWorkbenchCommands(
      baseInput({
        activeStatus: 'idle',
        isAutoDetecting: true,
      })
    );
    expect(detectingCmds.detectAutoRegion.enabled).toBe(false);
    expect(detectingCmds.startExtraction.enabled).toBe(false);

    const idleCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'idle', activeCueCount: 0 }));
    expect(idleCmds.startExtraction.enabled).toBe(true);
    expect(idleCmds.editCue.enabled).toBe(false);
    expect(idleCmds.exportActiveSrt.enabled).toBe(false);

    const extractingCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'extracting' }));
    expect(extractingCmds.detectAutoRegion.enabled).toBe(false);
    expect(extractingCmds.cancelExtraction.enabled).toBe(true);
    expect(extractingCmds.undo.enabled).toBe(false);

    const readyCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'ready' }));
    expect(readyCmds.editCue.enabled).toBe(true);
    expect(readyCmds.exportActiveSrt.enabled).toBe(true);

    const exportedCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'exported' }));
    expect(exportedCmds.editCue.enabled).toBe(true);
    expect(exportedCmds.exportActiveSrt.enabled).toBe(true);

    const cancelledCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'cancelled' }));
    expect(cancelledCmds.startExtraction.enabled).toBe(true);
    expect(cancelledCmds.editCue.enabled).toBe(false);
    expect(cancelledCmds.exportActiveSrt.enabled).toBe(false);

    const failedCmds = selectWorkbenchCommands(baseInput({ activeStatus: 'failed' }));
    expect(failedCmds.startExtraction.enabled).toBe(true);
    expect(failedCmds.editCue.enabled).toBe(false);
    expect(failedCmds.exportActiveSrt.enabled).toBe(false);
  });

  it('builds encoded sublift:// media URLs cleanly', () => {
    expect(buildMediaUrl('/Users/test/video 01.mkv')).toBe(
      'sublift://localhost/media?path=%2FUsers%2Ftest%2Fvideo%2001.mkv'
    );
    expect(buildMediaUrl('/tmp/a.mkv', 12.345)).toBe(
      'sublift://localhost/media?path=%2Ftmp%2Fa.mkv&t=12.345'
    );
    expect(buildMediaUrl('C:\\v\\a.mp4', undefined, 'windows')).toBe(
      'http://sublift.localhost/media?path=C%3A%5Cv%5Ca.mp4'
    );
  });
});
