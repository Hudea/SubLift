import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import { formatSrt } from '../utils/srt_formatter';
import type { SseJobCallbacks } from '../types/api';

describe('useWorkbenchStore - Milestone 5 (Feature 12515: 字幕审阅草稿与无损校对)', () => {
  let localStorageMock: Record<string, string> = {};

  beforeEach(() => {
    setActivePinia(createPinia());
    localStorageMock = {};

    // Mock localStorage
    vi.stubGlobal('localStorage', {
      getItem: (key: string) => localStorageMock[key] ?? null,
      setItem: (key: string, val: string) => {
        localStorageMock[key] = String(val);
      },
      removeItem: (key: string) => {
        delete localStorageMock[key];
      },
      clear: () => {
        localStorageMock = {};
      },
    });

    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0',
      runtime: 'cpp',
      capabilities: [],
      engines: [{ name: 'vision', available: true, detail: 'Apple Vision' }],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  describe('1. Versioned Draft State Machine & Auto-Save / Reload Restoration', () => {
    it('initializes draft lifecycle status cleanly and tracks transitions', () => {
      const store = useWorkbenchStore();
      expect(store.draftStatus).toBe('saved');
      expect(store.draftSavedAt).toBeNull();
      expect(store.hasUserEdits).toBe(false);
    });

    it('persists draft to localStorage on edit and updates draft status', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/sample_movie.mp4');

      // Seed initial entry
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Initial line', confidence: 0.95 },
      ];
      store.saveDraftNow();
      expect(store.draftStatus).toBe('saved');
      expect(store.draftSavedAt).not.toBeNull();

      // Perform text edit
      store.updateSubtitleEntry(1, { text: 'Edited subtitle text' });
      expect(store.draftStatus).toBe('dirty');
      expect(store.hasUserEdits).toBe(true);

      // Trigger immediate save
      const saved = store.saveDraftNow();
      expect(saved).toBe(true);
      expect(store.draftStatus).toBe('saved');

      // Inspect localStorage raw data
      const storageKey = store.getDraftStorageKey('/workspace/sample_movie.mp4');
      const raw = localStorage.getItem(storageKey);
      expect(raw).not.toBeNull();
      const parsed = JSON.parse(raw!);
      expect(parsed.version).toBe(1);
      expect(parsed.videoPath).toBe('/workspace/sample_movie.mp4');
      expect(parsed.entries[0].text).toBe('Edited subtitle text');
    });

    it('automatically restores persisted draft on reload/reopening video', () => {
      const storageKey = 'sublift_draft:/workspace/movie_with_draft.mp4';
      localStorage.setItem(
        storageKey,
        JSON.stringify({
          version: 1,
          videoPath: '/workspace/movie_with_draft.mp4',
          savedAt: 1700000000000,
          entries: [
            { index: 1, start_ms: 2000, end_ms: 4000, text: 'Restored draft line 1', confidence: 0.98 },
            { index: 2, start_ms: 4500, end_ms: 6000, text: 'Restored draft line 2', confidence: 0.91 },
          ],
        })
      );

      const store = useWorkbenchStore();
      store.loadVideo('/workspace/movie_with_draft.mp4');

      expect(store.entries.length).toBe(2);
      expect(store.entries[0].text).toBe('Restored draft line 1');
      expect(store.entries[1].text).toBe('Restored draft line 2');
      expect(store.state).toBe('Review');
      expect(store.draftStatus).toBe('saved');
      expect(store.hasUserEdits).toBe(true);
    });

    it('handles localStorage quota/failure gracefully with status failed', () => {
      vi.stubGlobal('localStorage', {
        getItem: () => null,
        setItem: () => {
          throw new Error('QuotaExceededError');
        },
        removeItem: () => {},
        clear: () => {},
      });

      const store = useWorkbenchStore();
      store.loadVideo('/workspace/test_quota.mp4');
      store.entries = [{ index: 1, start_ms: 1000, end_ms: 2000, text: 'Test', confidence: 1.0 }];

      const result = store.saveDraftNow();
      expect(result).toBe(false);
      expect(store.draftStatus).toBe('failed');
      expect(store.draftError).toContain('QuotaExceededError');
    });

    it('discards draft and resets cleanly', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/test_discard.mp4');
      store.entries = [{ index: 1, start_ms: 1000, end_ms: 2000, text: 'To Discard', confidence: 1.0 }];
      store.saveDraftNow();

      expect(store.hasPersistedDraft('/workspace/test_discard.mp4')).toBe(true);

      store.discardDraft('/workspace/test_discard.mp4');
      expect(store.hasPersistedDraft('/workspace/test_discard.mp4')).toBe(false);
      expect(store.entries.length).toBe(0);
      expect(store.hasUserEdits).toBe(false);
      expect(store.state).toBe('Ready');
    });
  });

  describe('2. Predictable Undo / Redo Command History Stack', () => {
    it('records history and undos/redos subtitle text edits', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/undo_test.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Version 0', confidence: 0.9 },
      ];

      expect(store.canUndo).toBe(false);
      expect(store.canRedo).toBe(false);

      // Edit 1
      store.updateSubtitleEntry(1, { text: 'Version 1' });
      expect(store.entries[0].text).toBe('Version 1');
      expect(store.canUndo).toBe(true);
      expect(store.canRedo).toBe(false);

      // Edit 2
      store.updateSubtitleEntry(1, { text: 'Version 2' });
      expect(store.entries[0].text).toBe('Version 2');

      // Undo -> Version 1
      expect(store.undo()).toBe(true);
      expect(store.entries[0].text).toBe('Version 1');
      expect(store.canRedo).toBe(true);

      // Undo -> Version 0
      expect(store.undo()).toBe(true);
      expect(store.entries[0].text).toBe('Version 0');
      expect(store.canUndo).toBe(false);

      // Redo -> Version 1
      expect(store.redo()).toBe(true);
      expect(store.entries[0].text).toBe('Version 1');

      // Redo -> Version 2
      expect(store.redo()).toBe(true);
      expect(store.entries[0].text).toBe('Version 2');
      expect(store.canRedo).toBe(false);
    });

    it('records history and undos/redos timecode edits', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/time_undo.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Time line', confidence: 0.9 },
      ];

      store.updateSubtitleEntry(1, { start_ms: 1500, end_ms: 3500 });
      expect(store.entries[0].start_ms).toBe(1500);
      expect(store.entries[0].end_ms).toBe(3500);

      store.undo();
      expect(store.entries[0].start_ms).toBe(1000);
      expect(store.entries[0].end_ms).toBe(3000);

      store.redo();
      expect(store.entries[0].start_ms).toBe(1500);
      expect(store.entries[0].end_ms).toBe(3500);
    });

    it('records history and undos/redos entry insertion and deletion', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/insert_undo.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 2000, text: 'First', confidence: 0.9 },
      ];

      // Insert
      const newEntry = store.insertEntryAfter(1);
      expect(newEntry).not.toBeNull();
      expect(store.entries.length).toBe(2);

      // Undo insert
      store.undo();
      expect(store.entries.length).toBe(1);
      expect(store.entries[0].text).toBe('First');

      // Redo insert
      store.redo();
      expect(store.entries.length).toBe(2);

      // Delete entry
      store.removeSubtitleEntry(1);
      expect(store.entries.length).toBe(1);

      // Undo delete
      store.undo();
      expect(store.entries.length).toBe(2);
      expect(store.entries[0].text).toBe('First');
    });

    it('records history and undos/redos mergeWithNext', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/merge_undo.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 2000, text: 'Hello', confidence: 0.9 },
        { index: 2, start_ms: 2100, end_ms: 3500, text: 'World', confidence: 0.8 },
      ];

      store.mergeWithNext(1);
      expect(store.entries.length).toBe(1);
      expect(store.entries[0].text).toBe('Hello World');
      expect(store.entries[0].end_ms).toBe(3500);

      // Undo merge
      store.undo();
      expect(store.entries.length).toBe(2);
      expect(store.entries[0].text).toBe('Hello');
      expect(store.entries[1].text).toBe('World');

      // Redo merge
      store.redo();
      expect(store.entries.length).toBe(1);
      expect(store.entries[0].text).toBe('Hello World');
    });

    it('clears redo stack when a new mutation branch occurs after undo', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/branch_undo.mp4');
      store.entries = [{ index: 1, start_ms: 1000, end_ms: 2000, text: 'A', confidence: 1.0 }];

      store.updateSubtitleEntry(1, { text: 'B' });
      store.updateSubtitleEntry(1, { text: 'C' });

      store.undo(); // now 'B'
      expect(store.canRedo).toBe(true);

      // New action creates a new history branch
      store.updateSubtitleEntry(1, { text: 'D' });
      expect(store.entries[0].text).toBe('D');
      expect(store.canRedo).toBe(false); // Redo stack was cleared!
    });
  });

  describe('3. Ownership Separation & Late Event Isolation', () => {
    it('drops late SSE onPushEntry events when user is in Review mode or has draft edits', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/isolation.mp4');

      const sseHolder: { current: SseJobCallbacks | null } = { current: null };
      SubLiftApiClient.createJob = async () => ({ job_id: 'iso-job', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, cb) => {
        sseHolder.current = cb;
        return () => {};
      };

      await store.startExtraction();
      expect(store.state).toBe('Processing');

      // Normal event during extraction
      sseHolder.current?.onPushEntry?.({
        entry: { index: 1, start_ms: 1000, end_ms: 2000, text: 'Original line', confidence: 0.95 },
      });
      expect(store.entries.length).toBe(1);

      // Complete extraction -> state transitions to Review
      sseHolder.current?.onDone?.({ total_entries: 1, elapsed_ms: 500 });
      expect(store.state).toBe('Review');

      // User performs custom review edits
      store.updateSubtitleEntry(1, { text: 'User human-corrected text' });
      expect(store.entries[0].text).toBe('User human-corrected text');

      // Late arriving SSE event from backend
      sseHolder.current?.onPushEntry?.({
        entry: { index: 2, start_ms: 2500, end_ms: 3500, text: 'Late engine entry', confidence: 0.8 },
      });

      // Late event must NOT be appended or overwrite user review draft
      expect(store.entries.length).toBe(1);
      expect(store.entries[0].text).toBe('User human-corrected text');
    });

    it('requires explicit confirmation (force: true) to re-run extraction over an existing draft', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/reextract.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 2000, text: 'Existing draft text', confidence: 1.0 },
      ];
      store.hasUserEdits = true;
      store.state = 'Review';

      let createJobCalled = false;
      SubLiftApiClient.createJob = async () => {
        createJobCalled = true;
        return { job_id: 'reextract-job', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = () => () => {};

      // Attempting extraction without force throws clear error to protect user draft
      await expect(store.startExtraction()).rejects.toThrow('当前存在已编辑的字幕审阅草稿，重新提取需要显式确认');
      expect(createJobCalled).toBe(false);

      // Passing force: true proceeds and clears prior draft
      await store.startExtraction({ force: true });
      expect(createJobCalled).toBe(true);
      expect(store.state).toBe('Processing');
    });
  });

  describe('4. Export Integrity with Latest Confirmed Draft Entries', () => {
    it('formats SRT from latest edited and sorted draft entries', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/export_test.mp4');

      store.entries = [
        { index: 2, start_ms: 4000, end_ms: 6000, text: 'Second sentence', confidence: 0.9 },
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'First sentence (edited)', confidence: 0.95 },
      ];

      const srtText = formatSrt(store.entries);
      expect(srtText).toContain('1\n00:00:01,000 --> 00:00:03,000\nFirst sentence (edited)');
      expect(srtText).toContain('2\n00:00:04,000 --> 00:00:06,000\nSecond sentence');
    });
  });
});
