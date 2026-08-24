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

  it('verifies BatchTaskStatusGuard isTerminal and nextActiveStage methods', () => {
    expect(BatchTaskStatusGuard.isTerminal('completed')).toBe(true);
    expect(BatchTaskStatusGuard.isTerminal('failed')).toBe(true);
    expect(BatchTaskStatusGuard.isTerminal('cancelled')).toBe(true);
    expect(BatchTaskStatusGuard.isTerminal('interrupted')).toBe(true);
    expect(BatchTaskStatusGuard.isTerminal('skipped')).toBe(true);
    expect(BatchTaskStatusGuard.isTerminal('waiting')).toBe(false);
    expect(BatchTaskStatusGuard.isTerminal('extracting')).toBe(false);

    expect(BatchTaskStatusGuard.nextActiveStage('preparing')).toBe('extracting');
    expect(BatchTaskStatusGuard.nextActiveStage('extracting')).toBe('exporting');
    expect(BatchTaskStatusGuard.nextActiveStage('exporting')).toBeNull();
    expect(BatchTaskStatusGuard.nextActiveStage('waiting')).toBeNull();
  });

  it('scanDataTransferItems handles FileList with webkitRelativePath for folder structure and skips hidden files', async () => {
    const fileList = [
      { name: 'movie.mp4', webkitRelativePath: 'Season1/movie.mp4', size: 1000 },
      { name: '.DS_Store', webkitRelativePath: 'Season1/.DS_Store', size: 50 },
      { name: 'app_asset.mp4', webkitRelativePath: 'App.app/Contents/app_asset.mp4', size: 200 },
      { name: 'notes.txt', webkitRelativePath: 'Season1/notes.txt', size: 100 },
    ] as unknown as FileList;

    const result = await BrowserDirectoryScanner.scanDataTransferItems(fileList);
    expect(result.files.length).toBe(1);
    expect(result.files[0].file.name).toBe('movie.mp4');
    expect(result.files[0].relativeDir).toBe('Season1/');
    expect(result.skipped).toBe(2); // .DS_Store and App.app
    expect(result.rejected.length).toBe(1); // notes.txt unsupportedFormat
    expect(result.rejected[0].reason.kind).toBe('unsupportedFormat');
  });

  it('resolveFilesWithConcurrency resolves native path and deduplicates', async () => {
    const mockFiles = [
      { file: { name: 'video1.mp4', path: '/local/media/video1.mp4', size: 1024 } as any },
      { file: { name: 'video2.mp4', path: '/local/media/video2.mp4', size: 2048 } as any, relativeDir: 'Clips/' },
    ];

    const existing = new Set<string>(['/local/media/video1.mp4']);
    const result = await BrowserDirectoryScanner.resolveFilesWithConcurrency(mockFiles, existing);

    expect(result.accepted.length).toBe(1);
    expect(result.accepted[0].videoPath).toBe('/local/media/video2.mp4');
    expect(result.accepted[0].importRootPath).toBe('Clips/');
    expect(result.rejected.length).toBe(1);
    expect(result.rejected[0].reason.kind).toBe('duplicate');
  });
});
