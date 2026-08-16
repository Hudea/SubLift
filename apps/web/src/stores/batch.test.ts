import { setActivePinia, createPinia } from 'pinia';
import { useBatchStore } from './batch';
import { SubLiftApiClient } from '../api/client';

function assert(condition: boolean, msg: string) {
  if (!condition) {
    throw new Error(`Assertion failed: ${msg}`);
  }
}

async function runTests() {
  console.log('--- Testing Batch Queue State Machine & Async Concurrency (Feature 12301) ---');
  setActivePinia(createPinia());
  const batchStore = useBatchStore();

  // 1. Initial State
  assert(batchStore.tasks.length === 0, 'Initial tasks should be empty');
  assert(batchStore.stats.total === 0, 'Stats total should be 0');
  assert(!batchStore.isQueueRunning, 'Queue should be initially idle');
  assert(!batchStore.canStartAll, 'Cannot start all on empty queue');

  // 2. Add multiple files
  batchStore.addFiles(['/path/to/video1.mp4', '/path/to/video2.mov', '/path/to/video3.webm']);
  assert(batchStore.tasks.length === 3, `Expected 3 tasks, got ${batchStore.tasks.length}`);
  assert(batchStore.stats.waiting === 3, 'All 3 tasks should be waiting');
  assert(batchStore.canStartAll, 'canStartAll should be true with waiting tasks');
  assert(batchStore.tasks[0].name === 'video1.mp4', 'Task 1 name mismatch');
  assert(batchStore.tasks[1].name === 'video2.mov', 'Task 2 name mismatch');
  console.log('✓ Batch add files and initial stats verified');

  // 3. Deduplication: adding existing waiting path should be ignored
  batchStore.addFiles(['/path/to/video1.mp4']);
  assert(batchStore.tasks.length === 3, 'Duplicate video path should not create extra task');
  console.log('✓ Deduplication verified');

  // 4. Cancel a waiting task
  const task2Id = batchStore.tasks[1].id;
  await batchStore.cancelTask(task2Id);
  assert(batchStore.tasks[1].status === 'cancelled', 'Task 2 should be cancelled');
  assert(batchStore.stats.waiting === 2, 'Waiting count should decrease to 2');
  assert(batchStore.stats.cancelled === 1, 'Cancelled count should be 1');
  console.log('✓ Task cancellation verified');

  // 5. Retry a cancelled task
  batchStore.retryTask(task2Id);
  assert(batchStore.tasks[1].status === 'waiting', 'Task 2 should return to waiting');
  assert(batchStore.stats.waiting === 3, 'Waiting count should return to 3');
  assert(batchStore.stats.cancelled === 0, 'Cancelled count should return to 0');
  console.log('✓ Task retry verified');

  // 6. Mock Async Single-Concurrency Queue Execution
  console.log('--- Testing Async Serial Worker Dispatch & Event Stream ---');
  let activeSseCallback: any = null;
  const createdJobIds: string[] = [];

  // Mock ApiClient
  SubLiftApiClient.createJob = async () => {
    const jobId = `mock-job-${createdJobIds.length + 1}`;
    createdJobIds.push(jobId);
    return { job_id: jobId, status: 'running' };
  };

  SubLiftApiClient.subscribeJobEvents = (_jobId, callbacks) => {
    activeSseCallback = callbacks;
    return () => {
      activeSseCallback = null;
    };
  };

  // Start Queue: Task 1 should start, Task 2 & 3 must remain waiting
  batchStore.startQueue();
  // Allow microtasks to resolve
  await new Promise((r) => setTimeout(r, 10));

  assert(batchStore.isQueueRunning, 'Queue should be running');
  assert(batchStore.currentRunningId === batchStore.tasks[0].id, 'Task 1 should be currentRunningId');
  assert(batchStore.tasks[0].status === 'running', 'Task 1 should be running');
  assert(batchStore.tasks[1].status === 'waiting', 'Task 2 MUST remain waiting (single concurrency invariant)');
  assert(batchStore.tasks[2].status === 'waiting', 'Task 3 MUST remain waiting');
  assert(batchStore.stats.running === 1, 'Exactly 1 task should be running');

  // Trigger double startQueue (should be safely ignored by mutex)
  batchStore.startQueue();
  await new Promise((r) => setTimeout(r, 10));
  assert(batchStore.stats.running === 1, 'Double startQueue must not spawn second task');
  console.log('✓ Serial execution & single concurrency invariant verified');

  // Simulate progress on Task 1
  activeSseCallback?.onProgress?.({ stage: 'ocr', pct: 0.65, eta_ms: 1200 });
  assert(batchStore.tasks[0].progressPct === 65, 'Task 1 progressPct should be 65%');
  assert(batchStore.tasks[0].stage === 'ocr', 'Task 1 stage should be ocr');

  // Simulate Task 1 Done -> Task 2 should automatically start
  activeSseCallback?.onDone?.({
    job_id: createdJobIds[0],
    status: 'completed',
    total_entries: 5,
    elapsed_ms: 3200,
    video_path: '/path/to/video1.mp4',
  });
  await new Promise((r) => setTimeout(r, 10));

  assert(batchStore.tasks[0].status === 'completed', 'Task 1 should be completed');
  assert(batchStore.tasks[0].progressPct === 100, 'Task 1 progressPct should be 100%');
  assert(batchStore.tasks[1].status === 'running', 'Task 2 should automatically start running');
  assert(batchStore.currentRunningId === batchStore.tasks[1].id, 'Current running should be Task 2');
  console.log('✓ Auto-advance to Task 2 on Task 1 Done verified');

  // 7. Fail-closed: Simulate Task 2 Error -> Task 3 should automatically start without stalling queue
  activeSseCallback?.onError?.('Corrupted video header');
  await new Promise((r) => setTimeout(r, 10));

  assert(batchStore.tasks[1].status === 'failed', 'Task 2 should be marked failed');
  assert(batchStore.tasks[1].error === 'Corrupted video header', 'Task 2 error message preserved');
  assert(batchStore.tasks[2].status === 'running', 'Task 3 should automatically start running (Fail-closed isolation)');
  console.log('✓ Fail-closed isolation on error verified');

  // 8. Cancel running task (Task 3)
  let cancelledJobId: string | null = null;
  SubLiftApiClient.cancelJob = async (jobId) => {
    cancelledJobId = jobId;
  };

  await batchStore.cancelTask(batchStore.tasks[2].id);
  await new Promise((r) => setTimeout(r, 10));

  assert(batchStore.tasks[2].status === 'cancelled', 'Task 3 should be cancelled');
  assert(cancelledJobId === createdJobIds[2], 'SubLiftApiClient.cancelJob should be called with correct jobId');
  assert(batchStore.currentRunningId === null, 'Current running should be null after queue completes');
  assert(!batchStore.isQueueRunning, 'Queue should be finished and idle');
  console.log('✓ In-flight task cancellation verified');

  // 9. Clear completed/cancelled
  assert(batchStore.stats.completed === 1, 'Stats completed should be 1');
  assert(batchStore.stats.failed === 1, 'Stats failed should be 1');
  assert(batchStore.stats.cancelled === 1, 'Stats cancelled should be 1');
  // 10. Test createJob throwing error immediately (Feature 12503 deadlock elimination)
  console.log('--- Testing Immediate createJob Failure Auto-Advancement (Feature 12503) ---');
  batchStore.clearAll();
  batchStore.addFiles(['/path/to/bad_video.mp4', '/path/to/good_video.mp4']);
  assert(batchStore.tasks.length === 2, 'Should have 2 tasks');

  // Configure createJob to throw for the first task and succeed for the second
  let callCount = 0;
  SubLiftApiClient.createJob = async () => {
    callCount++;
    if (callCount === 1) {
      throw new Error('400 Bad Request: Invalid media file');
    }
    return { job_id: 'good-job-id', status: 'running' };
  };

  batchStore.startQueue();
  // Allow microtasks to resolve for both the initial task and the next task
  await new Promise((r) => setTimeout(r, 20));

  assert(batchStore.tasks[0].status === 'failed', 'Task 1 should be failed immediately');
  assert(batchStore.tasks[0].error === '400 Bad Request: Invalid media file', 'Task 1 should hold error message');
  assert(batchStore.tasks[1].status === 'running', 'Task 2 MUST automatically start running (Deadlock Eliminated)');
  assert(batchStore.currentRunningId === batchStore.tasks[1].id, 'Current running should be Task 2');
  console.log('✓ createJob throw auto-advancement & deadlock elimination verified');

  console.log('All Feature 12301 & 12503 tests passed with 100% success!');
}

runTests();
