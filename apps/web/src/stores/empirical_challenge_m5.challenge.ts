import { describe, it, expect, beforeEach, vi, afterEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import {
  validateTimecodeString,
  detectTimelineConflicts,
} from '../utils/timecode';
import type { SubtitleEntry, SseJobCallbacks } from '../types/api';

describe('Empirical Challenge 1 - Phase 12 Milestone 5 (Feature 12515: 字幕审阅草稿与无损校对)', () => {
  let localStorageMock: Record<string, string> = {};

  beforeEach(() => {
    setActivePinia(createPinia());
    localStorageMock = {};

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
      version: 'v0.1.0-challenge',
      runtime: 'cpp',
      capabilities: ['mock', 'vision'],
      engines: [
        { name: 'mock', available: true, detail: 'Mock Engine' },
        { name: 'vision', available: true, detail: 'Apple Vision' },
      ],
      ffmpeg: { available: true, path: '/usr/bin/ffmpeg' },
    };
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  describe('1. Extreme Undo / Redo Sequences & History Stack Stress Harness', () => {
    it('handles 60+ sequential mutations, respects maxHistorySize=50 cap, and undos past beginning safely', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/stress_test.mp4');

      // Seed initial 3 entries
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Base 1', confidence: 0.9 },
        { index: 2, start_ms: 3500, end_ms: 5500, text: 'Base 2', confidence: 0.9 },
        { index: 3, start_ms: 6000, end_ms: 8000, text: 'Base 3', confidence: 0.9 },
      ];

      // Perform 60 mutations mixing text updates, insertions, deletions, merges, timecode updates
      const snapshotsAfterMutation: SubtitleEntry[][] = [];

      for (let i = 1; i <= 60; i++) {
        const mod = i % 5;
        if (mod === 0) {
          // Insert after first available entry
          const firstIdx = store.entries[0]?.index ?? 1;
          store.insertEntryAfter(firstIdx);
        } else if (mod === 1 && store.entries.length > 2) {
          // Merge first with second
          store.mergeWithNext(store.entries[0].index);
        } else if (mod === 2 && store.entries.length > 3) {
          // Delete last entry
          const lastIdx = store.entries[store.entries.length - 1].index;
          store.removeSubtitleEntry(lastIdx);
        } else if (mod === 3) {
          // Update timecode of first entry
          const target = store.entries[0];
          if (target) {
            store.updateSubtitleEntry(target.index, {
              start_ms: target.start_ms + 100,
              end_ms: target.end_ms + 200,
            });
          }
        } else {
          // Update text of first entry
          const target = store.entries[0];
          if (target) {
            store.updateSubtitleEntry(target.index, {
              text: `Mutation Step ${i} - ${Date.now()}`,
            });
          }
        }

        snapshotsAfterMutation.push(JSON.parse(JSON.stringify(store.entries)));
      }

      // History stack size must be bounded by maxHistorySize (50)
      expect(store.undoStack.length).toBe(50);
      expect(store.canUndo).toBe(true);
      expect(store.canRedo).toBe(false);

      // Snapshot before starting undos
      const headState = JSON.parse(JSON.stringify(store.entries));
      expect(headState).toEqual(snapshotsAfterMutation[59]);

      // Perform 50 undos -> should step back smoothly
      for (let u = 0; u < 50; u++) {
        const success = store.undo();
        expect(success).toBe(true);
        expect(store.redoStack.length).toBe(u + 1);
      }

      // Now undoStack is empty
      expect(store.undoStack.length).toBe(0);
      expect(store.canUndo).toBe(false);
      expect(store.canRedo).toBe(true);

      // State at bottom of history corresponds to state after mutation step 10 (since steps 1..10 were dropped by depth 50)
      const bottomState = JSON.parse(JSON.stringify(store.entries));
      expect(bottomState).toEqual(snapshotsAfterMutation[9]);

      // Attempting further undos past beginning must safely return false without corrupting entries or throwing
      for (let extra = 0; extra < 10; extra++) {
        expect(store.undo()).toBe(false);
        expect(store.canUndo).toBe(false);
        expect(store.undoStack.length).toBe(0);
      }
      expect(store.entries).toEqual(bottomState);

      // Perform 50 redos -> should step all the way back to headState
      for (let r = 0; r < 50; r++) {
        const success = store.redo();
        expect(success).toBe(true);
        expect(store.undoStack.length).toBe(r + 1);
      }

      expect(store.redoStack.length).toBe(0);
      expect(store.canRedo).toBe(false);
      expect(store.canUndo).toBe(true);
      expect(store.entries).toEqual(headState);

      // Attempting further redos past head must safely return false
      for (let extra = 0; extra < 10; extra++) {
        expect(store.redo()).toBe(false);
        expect(store.canRedo).toBe(false);
      }
      expect(store.entries).toEqual(headState);
    });

    it('clears redo stack when branching edits occur after multi-step undos', () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/branch_test.mp4');

      store.entries = [{ index: 1, start_ms: 1000, end_ms: 2000, text: 'Step 0', confidence: 1.0 }];

      // Make 10 edits
      for (let i = 1; i <= 10; i++) {
        store.updateSubtitleEntry(1, { text: `Step ${i}` });
      }

      expect(store.undoStack.length).toBe(10);

      // Undo 4 steps -> current text is 'Step 6'
      for (let u = 0; u < 4; u++) {
        store.undo();
      }
      expect(store.entries[0].text).toBe('Step 6');
      expect(store.undoStack.length).toBe(6);
      expect(store.redoStack.length).toBe(4);
      expect(store.canRedo).toBe(true);

      // Introduce branch mutation
      store.updateSubtitleEntry(1, { text: 'Branched Step 7B' });
      expect(store.entries[0].text).toBe('Branched Step 7B');

      // Redo stack must be completely cleared
      expect(store.redoStack.length).toBe(0);
      expect(store.canRedo).toBe(false);
      expect(store.undoStack.length).toBe(7);

      // Undo back to Step 6
      expect(store.undo()).toBe(true);
      expect(store.entries[0].text).toBe('Step 6');

      // Redo back to Branched Step 7B
      expect(store.redo()).toBe(true);
      expect(store.entries[0].text).toBe('Branched Step 7B');
      expect(store.canRedo).toBe(false);
    });
  });

  describe('2. Draft Restoration, Corrupted Storage & Quota Exceeded Resiliency', () => {
    it('restores draft across simulated page reloads and maintains video path isolation', () => {
      // 1. Session 1: User works on video A
      const store1 = useWorkbenchStore();
      store1.loadVideo('/workspace/session_video_a.mp4');
      store1.entries = [
        { index: 1, start_ms: 1200, end_ms: 3400, text: 'Video A Subtitle', confidence: 0.95 },
      ];
      store1.updateSubtitleEntry(1, { text: 'Video A Subtitle (Edited)' });
      store1.saveDraftNow();

      expect(store1.hasPersistedDraft('/workspace/session_video_a.mp4')).toBe(true);

      // 2. Session 2: Simulating page reload by resetting Pinia
      setActivePinia(createPinia());
      const store2 = useWorkbenchStore();

      // Loading a different video (video B) should NOT load video A's draft
      store2.loadVideo('/workspace/session_video_b.mp4');
      expect(store2.entries.length).toBe(0);
      expect(store2.state).toBe('Ready');
      expect(store2.hasUserEdits).toBe(false);

      // Loading video A restores video A's saved draft
      store2.loadVideo('/workspace/session_video_a.mp4');
      expect(store2.entries.length).toBe(1);
      expect(store2.entries[0].text).toBe('Video A Subtitle (Edited)');
      expect(store2.entries[0].start_ms).toBe(1200);
      expect(store2.entries[0].end_ms).toBe(3400);
      expect(store2.state).toBe('Review');
      expect(store2.draftStatus).toBe('saved');
      expect(store2.hasUserEdits).toBe(true);
    });

    it('gracefully handles corrupted draft JSON and invalid draft schemas in localStorage without crashing', () => {
      const store = useWorkbenchStore();
      const videoPath = '/workspace/corrupted_video.mp4';
      const key = store.getDraftStorageKey(videoPath);

      // Case 1: Malformed JSON syntax
      localStorage.setItem(key, '{ "version": 1, "entries": [ { invalid json ...');
      expect(() => store.loadVideo(videoPath)).not.toThrow();
      expect(store.entries.length).toBe(0);
      expect(store.state).toBe('Ready');
      expect(store.hasPersistedDraft(videoPath)).toBe(false);

      // Case 2: Incompatible version (e.g. version 2)
      localStorage.setItem(
        key,
        JSON.stringify({
          version: 2,
          videoPath,
          entries: [{ index: 1, start_ms: 100, end_ms: 200, text: 'V2 schema', confidence: 1 }],
        })
      );
      expect(() => store.loadVideo(videoPath)).not.toThrow();
      expect(store.entries.length).toBe(0);
      expect(store.state).toBe('Ready');

      // Case 3: Entries is not an array
      localStorage.setItem(
        key,
        JSON.stringify({
          version: 1,
          videoPath,
          entries: 'not-an-array',
        })
      );
      expect(() => store.loadVideo(videoPath)).not.toThrow();
      expect(store.entries.length).toBe(0);

      // Case 4: Null or empty string
      localStorage.setItem(key, '');
      expect(() => store.loadVideo(videoPath)).not.toThrow();
      expect(store.entries.length).toBe(0);
    });

    it('catches and handles localStorage QuotaExceededError in immediate and debounced auto-save', async () => {
      vi.useFakeTimers();

      let quotaFail = true;
      vi.stubGlobal('localStorage', {
        getItem: (k: string) => localStorageMock[k] ?? null,
        setItem: (k: string, v: string) => {
          if (quotaFail) {
            const err = new DOMException('The quota has been exceeded.', 'QuotaExceededError');
            throw err;
          }
          localStorageMock[k] = v;
        },
        removeItem: (k: string) => {
          delete localStorageMock[k];
        },
        clear: () => {
          localStorageMock = {};
        },
      });

      const store = useWorkbenchStore();
      store.loadVideo('/workspace/quota_video.mp4');
      store.entries = [{ index: 1, start_ms: 1000, end_ms: 2000, text: 'Quota test', confidence: 1.0 }];

      // 1. Immediate save under quota failure
      const saveRes = store.saveDraftNow();
      expect(saveRes).toBe(false);
      expect(store.draftStatus).toBe('failed');
      expect(store.draftError).toContain('The quota has been exceeded');

      // 2. Debounced auto-save under quota failure
      store.updateSubtitleEntry(1, { text: 'Trigger debounced auto save' });
      expect(store.draftStatus).toBe('dirty');

      // Fast forward autoSaveTimer (300ms)
      vi.advanceTimersByTime(350);
      expect(store.draftStatus).toBe('failed');
      expect(store.draftError).toContain('The quota has been exceeded');

      // 3. Storage clears up and retry succeeds
      quotaFail = false;
      const retryRes = store.saveDraftNow();
      expect(retryRes).toBe(true);
      expect(store.draftStatus).toBe('saved');
      expect(store.draftError).toBeNull();

      vi.useRealTimers();
    });
  });

  describe('3. Event Race Condition: Concurrent Late SSE Events vs Active User Draft', () => {
    it('strictly isolates user draft from late SSE push_entry bursts during and after review editing', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/race_video.mp4');

      let sseCallbacks: SseJobCallbacks = {};
      SubLiftApiClient.createJob = async () => ({ job_id: 'race-job-1', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, cb) => {
        sseCallbacks = cb;
        return () => {
          sseCallbacks = {};
        };
      };

      await store.startExtraction();
      expect(store.state).toBe('Processing');

      // 1. Initial 2 valid entries arrived during extraction
      sseCallbacks.onPushEntry?.({
        entry: { index: 1, start_ms: 1000, end_ms: 2000, text: 'Engine Line 1', confidence: 0.95 },
      });
      sseCallbacks.onPushEntry?.({
        entry: { index: 2, start_ms: 2200, end_ms: 3800, text: 'Engine Line 2', confidence: 0.9 },
      });
      expect(store.entries.length).toBe(2);

      // 2. Extraction finishes -> transitions to Review state
      sseCallbacks.onDone?.({ total_entries: 2, elapsed_ms: 800 });
      expect(store.state).toBe('Review');

      // 3. User begins editing entry 1 and inserts a custom line
      store.updateSubtitleEntry(1, { text: 'User Corrected Line 1' });
      const inserted = store.insertEntryAfter(1);
      expect(inserted).not.toBeNull();
      inserted!.text = 'User Inserted Line 1.5';
      expect(store.entries.length).toBe(3);
      expect(store.hasUserEdits).toBe(true);

      const snapshotBeforeLateEvents = JSON.parse(JSON.stringify(store.entries));

      // 4. Dispatch a burst of 30 late concurrent SSE events from rogue/slow workers
      for (let i = 3; i <= 32; i++) {
        sseCallbacks.onPushEntry?.({
          entry: {
            index: i,
            start_ms: i * 2000,
            end_ms: i * 2000 + 1500,
            text: `Late Engine Line ${i}`,
            confidence: 0.85,
          },
        });
      }

      // 5. Verification: None of the late SSE events must have penetrated the draft
      expect(store.entries.length).toBe(3);
      expect(store.entries).toEqual(snapshotBeforeLateEvents);
      expect(store.entries[0].text).toBe('User Corrected Line 1');
      expect(store.entries[1].text).toBe('User Inserted Line 1.5');
      expect(store.entries[2].text).toBe('Engine Line 2');
    });

    it('safeguards active draft against accidental re-extraction without force flag', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/workspace/safe_reextract.mp4');
      store.entries = [
        { index: 1, start_ms: 1000, end_ms: 2500, text: 'Valuable Human Edits', confidence: 1.0 },
      ];
      store.hasUserEdits = true;
      store.state = 'Review';
      store.saveDraftNow();

      let createJobCalled = false;
      SubLiftApiClient.createJob = async () => {
        createJobCalled = true;
        return { job_id: 'safe-job', status: 'running' };
      };
      SubLiftApiClient.subscribeJobEvents = () => () => {};

      // Calling without force fails closed
      await expect(store.startExtraction()).rejects.toThrow(
        /当前存在已编辑的字幕审阅草稿，重新提取需要显式确认以避免覆盖草稿/
      );
      expect(createJobCalled).toBe(false);
      expect(store.entries[0].text).toBe('Valuable Human Edits');
      expect(store.hasPersistedDraft('/workspace/safe_reextract.mp4')).toBe(true);

      // Calling with force: true proceeds and resets draft cleanly
      await store.startExtraction({ force: true });
      expect(createJobCalled).toBe(true);
      expect(store.state).toBe('Processing');
      expect(store.entries.length).toBe(0);
      expect(store.hasUserEdits).toBe(false);
    });
  });

  describe('4. Timecode Validation & Timeline Conflict Detection Stress Matrix', () => {
    it('parses and validates various flexible separator formats correctly', () => {
      // Valid separators: -->, ->, ~, 至, -
      expect(validateTimecodeString('00:01.000 --> 00:03.500')).toEqual({
        valid: true,
        start_ms: 1000,
        end_ms: 3500,
      });

      expect(validateTimecodeString('01:00:00.000 -> 01:00:10.000')).toEqual({
        valid: true,
        start_ms: 3600000,
        end_ms: 3610000,
      });

      expect(validateTimecodeString('00:05.500 ~ 00:08.200')).toEqual({
        valid: true,
        start_ms: 5500,
        end_ms: 8200,
      });

      expect(validateTimecodeString('00:10.000 至 00:15.000')).toEqual({
        valid: true,
        start_ms: 10000,
        end_ms: 15000,
      });

      expect(validateTimecodeString('00:20.000 - 00:25.000')).toEqual({
        valid: true,
        start_ms: 20000,
        end_ms: 25000,
      });
    });

    it('rejects invalid, inverted, and malformed timecode strings with clear errors', () => {
      // Missing separator
      expect(validateTimecodeString('00:01.000 00:03.000').valid).toBe(false);
      expect(validateTimecodeString('00:01.000 00:03.000').error).toContain('缺少分隔符');

      // Empty string
      expect(validateTimecodeString('').valid).toBe(false);

      // Inverted range (start >= end)
      const inv = validateTimecodeString('00:05.000 - 00:02.000');
      expect(inv.valid).toBe(false);
      expect(inv.error).toContain('必须早于结束时间');

      // Zero-length range (start == end)
      const zero = validateTimecodeString('00:05.000 - 00:05.000');
      expect(zero.valid).toBe(false);
      expect(zero.error).toContain('必须早于结束时间');

      // Corrupted number tokens
      const badToken = validateTimecodeString('abc - 00:05.000');
      expect(badToken.valid).toBe(false);
      expect(badToken.error).toContain('开始时间码格式错误');
    });

    it('accurately identifies invalid durations and overlap conflicts across disordered entry sequences', () => {
      const disorderedEntries = [
        // Out of chronological order
        { index: 3, start_ms: 5000, end_ms: 7000, text: 'Line 3' },
        { index: 1, start_ms: 1000, end_ms: 3000, text: 'Line 1' },
        { index: 2, start_ms: 2500, end_ms: 4500, text: 'Line 2 (Overlaps Line 1 and 3)' },
        { index: 4, start_ms: 8000, end_ms: 8000, text: 'Line 4 (Zero duration)' },
      ];

      const conflicts = detectTimelineConflicts(disorderedEntries);

      // Line 1: overlaps next (Line 2)
      const c1 = conflicts.get(1);
      expect(c1).toBeDefined();
      expect(c1!.some((w) => w.type === 'overlap_next')).toBe(true);

      // Line 2: overlaps prev (Line 1)
      const c2 = conflicts.get(2);
      expect(c2).toBeDefined();
      expect(c2!.some((w) => w.type === 'overlap_prev')).toBe(true);

      // Line 3: starts at 5000, previous Line 2 ends at 4500 -> no overlap
      const c3 = conflicts.get(3);
      expect(c3).toBeUndefined();

      // Line 4: start_ms === end_ms (invalid duration)
      const c4 = conflicts.get(4);
      expect(c4).toBeDefined();
      expect(c4!.some((w) => w.type === 'invalid_duration')).toBe(true);
    });
  });
});
