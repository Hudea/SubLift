import type {
  SystemInfoDTO,
  JobConfig,
  CreateJobResponse,
  SseProgressData,
  SsePushEntryData,
  SseDoneData,
} from '../types/api';

const API_BASE = '/api';

export class SubLiftApiClient {
  static async getSystemInfo(): Promise<SystemInfoDTO> {
    const res = await fetch(`${API_BASE}/system/info`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to fetch system info: HTTP ${res.status}`);
    }
    return res.json();
  }

  static getVideoStreamUrl(videoPath: string): string {
    return `${API_BASE}/video/stream?path=${encodeURIComponent(videoPath)}`;
  }

  static getVideoFrameUrl(videoPath: string, timeS: number = 0): string {
    return `${API_BASE}/video/frame?path=${encodeURIComponent(videoPath)}&time_s=${timeS}`;
  }

  static async createJob(config: JobConfig): Promise<CreateJobResponse> {
    const res = await fetch(`${API_BASE}/jobs`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify(config),
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(err.error || `Failed to create job: HTTP ${res.status}`);
    }
    return res.json();
  }

  static async cancelJob(jobId: string): Promise<void> {
    const res = await fetch(`${API_BASE}/jobs/${encodeURIComponent(jobId)}/cancel`, {
      method: 'POST',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to cancel job: HTTP ${res.status}`);
    }
  }

  static async exportSrt(jobId: string): Promise<string> {
    const res = await fetch(`${API_BASE}/jobs/${encodeURIComponent(jobId)}/export`, {
      method: 'GET',
    });
    if (!res.ok) {
      throw new Error(`Failed to export SRT: HTTP ${res.status}`);
    }
    return res.text();
  }

  static subscribeJobEvents(
    jobId: string,
    callbacks: {
      onProgress?: (data: SseProgressData) => void;
      onPushEntry?: (data: SsePushEntryData) => void;
      onDone?: (data: SseDoneData) => void;
      onError?: (err: string) => void;
    }
  ): () => void {
    const eventSource = new EventSource(`${API_BASE}/jobs/${encodeURIComponent(jobId)}/events`);

    if (callbacks.onProgress) {
      eventSource.addEventListener('progress', (e: MessageEvent) => {
        try {
          callbacks.onProgress!(JSON.parse(e.data));
        } catch (err) {
          console.error('[SSE] Failed to parse progress event', err);
        }
      });
    }

    if (callbacks.onPushEntry) {
      eventSource.addEventListener('push_entry', (e: MessageEvent) => {
        try {
          callbacks.onPushEntry!(JSON.parse(e.data));
        } catch (err) {
          console.error('[SSE] Failed to parse push_entry event', err);
        }
      });
    }

    if (callbacks.onDone) {
      eventSource.addEventListener('done', (e: MessageEvent) => {
        try {
          callbacks.onDone!(JSON.parse(e.data));
        } finally {
          eventSource.close();
        }
      });
    }

    if (callbacks.onError) {
      eventSource.addEventListener('error', (e: Event) => {
        const msgEvent = e as MessageEvent;
        if (msgEvent && msgEvent.data) {
          try {
            const data = JSON.parse(msgEvent.data);
            callbacks.onError!(data.error || 'Pipeline error');
          } catch {
            callbacks.onError!(String(msgEvent.data));
          }
        } else {
          // Native connection lost or closed unexpectedly
          callbacks.onError!('SSE connection lost or closed unexpectedly');
        }
      });
    }

    return () => {
      eventSource.close();
    };
  }
}
