import { describe, it, expect } from 'vitest';
import { BrowserDirectoryScanner } from './browser_scanner';
import { BatchTaskStatusGuard } from './batch_guards';

describe('BrowserDirectoryScanner & BatchTaskStatusGuard', () => {
  it('correctly classifies media extensions', () => {
    expect(BrowserDirectoryScanner.isSupportedMedia('video.mp4')).toBe(true);
    expect(BrowserDirectoryScanner.isSupportedMedia('clip.MOV')).toBe(true);
    expect(BrowserDirectoryScanner.isSupportedMedia('movie.mkv')).toBe(true);
    expect(BrowserDirectoryScanner.isSupportedMedia('stream.webm')).toBe(true);
    expect(BrowserDirectoryScanner.isSupportedMedia('doc.txt')).toBe(false);
    expect(BrowserDirectoryScanner.isSupportedMedia('sub.srt')).toBe(false);
    expect(BrowserDirectoryScanner.isSupportedMedia('.DS_Store')).toBe(false);
  });

  it('verifies BatchTaskStatusGuard legal transitions', () => {
    // waiting -> preparing/cancelled/skipped/interrupted
    expect(BatchTaskStatusGuard.canTransition('waiting', 'preparing')).toBe(true);
    expect(BatchTaskStatusGuard.canTransition('waiting', 'cancelled')).toBe(true);
    expect(BatchTaskStatusGuard.canTransition('waiting', 'completed')).toBe(false);

    // preparing -> extracting/failed/cancelled
    expect(BatchTaskStatusGuard.canTransition('preparing', 'extracting')).toBe(true);
    expect(BatchTaskStatusGuard.canTransition('preparing', 'failed')).toBe(true);

    // extracting -> exporting/failed/cancelled
    expect(BatchTaskStatusGuard.canTransition('extracting', 'exporting')).toBe(true);

    // exporting -> completed/failed/cancelled
    expect(BatchTaskStatusGuard.canTransition('exporting', 'completed')).toBe(true);

    // completed cannot transition anywhere
    expect(BatchTaskStatusGuard.canTransition('completed', 'waiting')).toBe(false);
    expect(BatchTaskStatusGuard.canTransition('completed', 'extracting')).toBe(false);
  });

  it('verifies BatchTaskStatusGuard command permissions', () => {
    expect(BatchTaskStatusGuard.canStartSingle('waiting')).toBe(true);
    expect(BatchTaskStatusGuard.canStartSingle('preparing')).toBe(false);

    expect(BatchTaskStatusGuard.canEditConfig('waiting')).toBe(true);
    expect(BatchTaskStatusGuard.canEditConfig('extracting')).toBe(false);
    expect(BatchTaskStatusGuard.canEditConfig('completed')).toBe(false);

    expect(BatchTaskStatusGuard.canReorder('waiting')).toBe(true);
    expect(BatchTaskStatusGuard.canReorder('extracting')).toBe(false);

    expect(BatchTaskStatusGuard.canRetry('failed')).toBe(true);
    expect(BatchTaskStatusGuard.canRetry('cancelled')).toBe(true);
    expect(BatchTaskStatusGuard.canRetry('interrupted')).toBe(true);
    expect(BatchTaskStatusGuard.canRetry('completed')).toBe(false);
    expect(BatchTaskStatusGuard.canRetry('waiting')).toBe(false);
  });

  it('loops readEntries until batch length is zero to prevent truncation', async () => {
    let callCount = 0;
    const mockReader: FileSystemDirectoryReader = {
      readEntries: (successCallback: (entries: FileSystemEntry[]) => void) => {
        callCount++;
        if (callCount === 1) {
          // First batch returns 2 items
          successCallback([
            { name: 'v1.mp4', isFile: true, isDirectory: false } as any,
            { name: 'v2.mov', isFile: true, isDirectory: false } as any,
          ]);
        } else if (callCount === 2) {
          // Second batch returns 1 item
          successCallback([
            { name: 'v3.mkv', isFile: true, isDirectory: false } as any,
          ]);
        } else {
          // Final batch returns empty array -> termination
          successCallback([]);
        }
      },
    };

    const entries = await BrowserDirectoryScanner.readAllDirectoryEntries(mockReader);
    expect(entries.length).toBe(3);
    expect(callCount).toBe(3);
  });
});
