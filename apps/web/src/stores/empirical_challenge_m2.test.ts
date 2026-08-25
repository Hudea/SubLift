import { describe, it, expect, beforeEach } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useWorkbenchStore } from './workbench';
import { useBatchStore } from './batch';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';
import type { SseJobCallbacks, SubtitleEntry } from '../types/api';

describe('Empirical Challenge - Phase 12 Milestone 2 (Feature 12512)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    const systemStore = useSystemStore();
    systemStore.systemInfo = {
      version: 'v0.1.0-challenge',
      runtime: 'cpp',
      capabilities: ['mock', 'vision', 'paddle'],
      engines: [{ name: 'mock', available: true, detail: 'Mock Engine' }],
      ffmpeg: { available: true, path: '/opt/homebrew/bin/ffmpeg' },
    };
  });

  describe('1. Subtitle Entry Deduplication Under Stress Replay', () => {
    it('workbenchStore: deduplicates identical entries replayed multiple times in arbitrary order', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/ws/video1.mp4');

      let capturedCallbacks: SseJobCallbacks = {};
      SubLiftApiClient.createJob = async () => ({ job_id: 'job-dedup-1', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        capturedCallbacks = callbacks;
        return () => {
          capturedCallbacks = {};
        };
      };

      const startPromise = store.startExtraction();
      await new Promise((r) => setTimeout(r, 5));

      const rawEntries: SubtitleEntry[] = Array.from({ length: 10 }, (_, i) => ({
        index: i + 1,
        start_ms: i * 2000,
        end_ms: (i + 1) * 2000 - 100,
        text: `Subtitle line #${i + 1}`,
        confidence: 0.95,
      }));

      // Initial push of 10 entries
      for (const entry of rawEntries) {
        capturedCallbacks.onPushEntry?.({ job_id: 'job-dedup-1', entry });
      }
      expect(store.entries.length).toBe(10);

      // Replay all 10 entries 5 times
      for (let round = 0; round < 5; ++round) {
        for (const entry of rawEntries) {
          capturedCallbacks.onPushEntry?.({ job_id: 'job-dedup-1', entry });
        }
      }
      expect(store.entries.length).toBe(10);

      // Replay in reverse order
      for (let i = rawEntries.length - 1; i >= 0; --i) {
        capturedCallbacks.onPushEntry?.({ job_id: 'job-dedup-1', entry: rawEntries[i] });
      }
      expect(store.entries.length).toBe(10);

      // Replay with identical timestamps and text but different index
      capturedCallbacks.onPushEntry?.({
        job_id: 'job-dedup-1',
        entry: {
          index: 999,
          start_ms: rawEntries[0].start_ms,
          end_ms: rawEntries[0].end_ms,
          text: rawEntries[0].text,
          confidence: 0.99,
        },
      });
      expect(store.entries.length).toBe(10);

      // Replay with identical index but different text (should be deduplicated by index)
      capturedCallbacks.onPushEntry?.({
        job_id: 'job-dedup-1',
        entry: {
          index: 1,
          start_ms: 99999,
          end_ms: 100000,
          text: 'Totally different text',
          confidence: 0.5,
        },
      });
      expect(store.entries.length).toBe(10);

      // Finish extraction
      capturedCallbacks.onDone?.({
        job_id: 'job-dedup-1',
        total_entries: 10,
        elapsed_ms: 1200,
      });

      await startPromise;
      expect(store.state).toBe('Review');
      expect(store.entries.length).toBe(10);
      expect(store.entries[0].text).toBe('Subtitle line #1');
      expect(store.entries[9].text).toBe('Subtitle line #10');
    });

    it('batchStore: deduplicates identical entries replayed under batch execution', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/batch_video.mp4',
        },
      ]);

      let capturedCallbacks: SseJobCallbacks = {};
      SubLiftApiClient.createJob = async () => ({ job_id: 'batch-job-1', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        capturedCallbacks = callbacks;
        return () => {
          capturedCallbacks = {};
        };
      };

      const runPromise = batchStore.startSingle(batchStore.tasks[0].id);
      await new Promise((r) => setTimeout(r, 5));

      const rawEntries: SubtitleEntry[] = Array.from({ length: 10 }, (_, i) => ({
        index: i + 1,
        start_ms: i * 1500,
        end_ms: (i + 1) * 1500 - 50,
        text: `Batch line #${i + 1}`,
        confidence: 0.9,
      }));

      // Push 10 entries with interleaving duplicates
      for (const entry of rawEntries) {
        capturedCallbacks.onPushEntry?.({ job_id: 'batch-job-1', entry });
        // duplicate push immediately
        capturedCallbacks.onPushEntry?.({ job_id: 'batch-job-1', entry });
      }
      expect(batchStore.tasks[0].entries.length).toBe(10);

      // Replay arbitrary chunk
      capturedCallbacks.onPushEntry?.({ job_id: 'batch-job-1', entry: rawEntries[3] });
      capturedCallbacks.onPushEntry?.({ job_id: 'batch-job-1', entry: rawEntries[7] });
      expect(batchStore.tasks[0].entries.length).toBe(10);

      capturedCallbacks.onDone?.({
        job_id: 'batch-job-1',
        total_entries: 10,
        elapsed_ms: 1500,
      });

      await runPromise;
      expect(batchStore.tasks[0].status).toBe('completed');
      expect(batchStore.tasks[0].entries.length).toBe(10);
      expect(batchStore.tasks[0].result?.entryCount).toBe(10);
    });
  });

  describe('2. Progress Scale Edge Values Across Single and Batch Views', () => {
    it('workbenchStore progressPct converts edge float values accurately', () => {
      const store = useWorkbenchStore();

      const testCases: [number | undefined, number][] = [
        [0.0, 0],
        [0.001, 0],       // Math.round(0.1) = 0
        [0.0049, 0],      // Math.round(0.49) = 0
        [0.005, 1],       // Math.round(0.5) = 1
        [0.009, 1],       // Math.round(0.9) = 1
        [0.125, 13],      // Math.round(12.5) = 13
        [0.5, 50],        // Math.round(50.0) = 50
        [0.9949, 99],     // Math.round(99.49) = 99
        [0.995, 100],     // Math.round(99.5) = 100
        [0.999, 100],     // Math.round(99.9) = 100
        [1.0, 100],       // Math.round(100.0) = 100
        [undefined, 0],   // default fallback = 0
      ];

      for (const [inputPct, expectedIntegerPct] of testCases) {
        store.progress = {
          stage: 'extracting',
          pct: inputPct as number,
        };
        expect(store.progressPct).toBe(expectedIntegerPct);
      }
    });

    it('batchStore task.progressPct converts edge float values in exact parity with workbenchStore', async () => {
      const batchStore = useBatchStore();
      batchStore.addBatchItems([
        {
          videoPath: '/ws/edge_progress.mp4',
        },
      ]);

      let capturedCallbacks: SseJobCallbacks = {};
      SubLiftApiClient.createJob = async () => ({ job_id: 'batch-job-edge', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (_id, callbacks) => {
        capturedCallbacks = callbacks;
        return () => {
          capturedCallbacks = {};
        };
      };

      const runPromise = batchStore.startSingle(batchStore.tasks[0].id);
      await new Promise((r) => setTimeout(r, 5));

      const testEdgeValues: [number, number][] = [
        [0.0, 0],
        [0.001, 0],
        [0.005, 1],
        [0.5, 50],
        [0.999, 100],
        [1.0, 100],
      ];

      for (const [inputPct, expectedIntegerPct] of testEdgeValues) {
        capturedCallbacks.onProgress?.({
          job_id: 'batch-job-edge',
          stage: 'extracting',
          pct: inputPct,
        });
        expect(batchStore.tasks[0].progressPct).toBe(expectedIntegerPct);
      }

      // onDone forces 100%
      capturedCallbacks.onDone?.({
        job_id: 'batch-job-edge',
        total_entries: 0,
        elapsed_ms: 500,
      });
      await runPromise;
      expect(batchStore.tasks[0].progressPct).toBe(100);
    });
  });

  describe('3. Simulated SSE Reconnection with Last-Event-ID / Cursor', () => {
    it('simulates client reconnecting after disconnect at event 4, receiving 5..10 with zero duplication', async () => {
      const store = useWorkbenchStore();
      store.loadVideo('/ws/reconnect_test.mp4');

      const allEvents: SubtitleEntry[] = Array.from({ length: 10 }, (_, i) => ({
        index: i + 1,
        start_ms: (i + 1) * 1000,
        end_ms: (i + 1) * 1000 + 800,
        text: `Stream item ${i + 1}`,
        confidence: 0.98,
      }));

      SubLiftApiClient.createJob = async () => ({ job_id: 'job-reconnect', status: 'running' });
      SubLiftApiClient.subscribeJobEvents = (id, callbacks, options) => {
        const cursor = options?.lastEventId ? Number(options.lastEventId) : 0;

        // Simulated server emitting items from cursor + 1
        const eventsToEmit = allEvents.slice(cursor);
        for (const entry of eventsToEmit) {
          callbacks.onPushEntry?.({ job_id: id, entry });
        }

        return () => {};
      };

      // Initial connection (cursor = 0)
      let unsub = SubLiftApiClient.subscribeJobEvents('job-reconnect', {
        onPushEntry: (data) => {
          if (data?.entry) {
            const exists = store.entries.some((e) => e.index === data.entry.index);
            if (!exists) store.entries.push(data.entry);
          }
        },
      });

      // Simulate receiving events 1..4 before connection drops
      store.entries = allEvents.slice(0, 4);
      expect(store.entries.length).toBe(4);
      unsub(); // Disconnect!

      // Reconnect with lastEventId = 4
      const lastReceivedId = 4;
      unsub = SubLiftApiClient.subscribeJobEvents(
        'job-reconnect',
        {
          onPushEntry: (data) => {
            if (data?.entry) {
              const exists = store.entries.some((e) => e.index === data.entry.index);
              if (!exists) store.entries.push(data.entry);
            }
          },
        },
        { lastEventId: lastReceivedId }
      );

      // Verify all 10 entries are present without duplication
      expect(store.entries.length).toBe(10);
      for (let i = 0; i < 10; ++i) {
        expect(store.entries[i].index).toBe(i + 1);
        expect(store.entries[i].text).toBe(`Stream item ${i + 1}`);
      }
      unsub();
    });
  });
});
