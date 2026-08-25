import type {
  SystemInfoDTO,
  JobConfig,
  CreateJobResponse,
  SseJobCallbacks,
  FileFingerprintDTO,
  WorkspaceConfigDTO,
  RegionDetectionDTO,
  JobSaveRequest,
  JobSaveResponse,
  BatchSaveRequest,
  BatchSaveResponse,
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

  /** 获取当前媒体工作区配置（Feature 12508） */
  static async getWorkspaceConfig(): Promise<WorkspaceConfigDTO> {
    const res = await fetch(`${API_BASE}/config/workspace`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to fetch workspace config: HTTP ${res.status}`);
    }
    return res.json();
  }

  /** 设置媒体工作区目录（Feature 12508） */
  static async setWorkspaceConfig(mediaDir: string): Promise<WorkspaceConfigDTO> {
    const res = await fetch(`${API_BASE}/config/workspace`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({ media_dir: mediaDir }),
    });
    if (!res.ok) {
      const errJson = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(errJson.error || `Failed to set workspace: HTTP ${res.status}`);
    }
    return res.json();
  }

  /** 清除媒体工作区配置（Feature 12508） */
  static async clearWorkspaceConfig(): Promise<WorkspaceConfigDTO> {
    const res = await fetch(`${API_BASE}/config/workspace/clear`, {
      method: 'POST',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to clear workspace: HTTP ${res.status}`);
    }
    return res.json();
  }

  /** 获取当前工作区下的所有可用视频文件（Feature 12508） */
  static async getWorkspaceVideos(): Promise<import('../types/api').WorkspaceVideoFileDTO[]> {
    const res = await fetch(`${API_BASE}/config/workspace/videos`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to fetch workspace videos: HTTP ${res.status}`);
    }
    return res.json();
  }

  static getVideoStreamUrl(videoPath: string): string {
    return `${API_BASE}/video/stream?path=${encodeURIComponent(videoPath)}`;
  }

  static getVideoFrameUrl(videoPath: string, timeS: number = 0): string {
    return `${API_BASE}/video/frame?path=${encodeURIComponent(videoPath)}&time_s=${timeS}`;
  }

  /**
   * 本机指纹反查（Feature 12507）：把浏览器侧文件的指纹交给服务端，
   * 在受控目录内定位同一文件的服务端路径。命中返回路径，未命中返回 null。
   */
  static async resolveVideoPath(fp: FileFingerprintDTO): Promise<string | null> {
    const res = await fetch(`${API_BASE}/video/resolve`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify(fp),
    });
    if (res.status === 404) return null;
    if (!res.ok) {
      throw new Error(`Failed to resolve video path: HTTP ${res.status}`);
    }
    const data = await res.json();
    return typeof data.path === 'string' && data.path ? data.path : null;
  }

  /**
   * 服务端绝对路径 / 目录递归扫描与展开 (Feature 12510)
   * 传入一个或多个绝对路径（文件或目录），服务端递归扫描并返回包含全部视频文件的结构化摘要。
   */
  static async scanServerPaths(paths: string[]): Promise<import('../types/batch').BatchScanSummary> {
    const res = await fetch(`${API_BASE}/video/scan-path`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({ paths }),
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(err.error || `Failed to scan server paths: HTTP ${res.status}`);
    }
    return res.json();
  }

  /**
   * 智能字幕区域自动识别 (Feature 12509)：
   * 对视频进行多点采样或单点截帧，识别字幕区域并返回推荐 ROI 选区。
   */
  static async detectSubtitleRegion(
    videoPath: string,
    timeSec?: number,
    engine?: string
  ): Promise<RegionDetectionDTO> {
    const res = await fetch(`${API_BASE}/video/detect-region`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify({
        video_path: videoPath,
        ...(timeSec !== undefined ? { time_s: timeSec } : {}),
        ...(engine ? { engine } : {}),
      }),
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(err.error || `Failed to detect subtitle region: HTTP ${res.status}`);
    }
    return res.json();
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

  /**
   * 服务端受控原子落盘保存单个任务字幕 (Feature 12514)
   */
  static async saveJobToDisk(
    jobId: string,
    options?: JobSaveRequest
  ): Promise<JobSaveResponse> {
    const res = await fetch(`${API_BASE}/jobs/${encodeURIComponent(jobId)}/save`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify(options || {}),
    });
    if (!res.ok) {
      const errJson = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(errJson.error || `Failed to save subtitles: HTTP ${res.status}`);
    }
    return res.json();
  }

  /**
   * 服务端受控原子批量落盘保存字幕 (Feature 12514)
   */
  static async batchSaveToDisk(
    options?: BatchSaveRequest
  ): Promise<BatchSaveResponse> {
    const res = await fetch(`${API_BASE}/export/batch-save`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Accept: 'application/json',
      },
      body: JSON.stringify(options || {}),
    });
    if (!res.ok) {
      const errJson = await res.json().catch(() => ({ error: `HTTP ${res.status}` }));
      throw new Error(errJson.error || `Failed to batch save subtitles: HTTP ${res.status}`);
    }
    return res.json();
  }

  static async getJobs(): Promise<import('../types/api').JobDetailDTO[]> {
    const res = await fetch(`${API_BASE}/jobs`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to fetch jobs: HTTP ${res.status}`);
    }
    return res.json();
  }

  static async getJob(jobId: string): Promise<import('../types/api').JobDetailDTO> {
    const res = await fetch(`${API_BASE}/jobs/${encodeURIComponent(jobId)}`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
    });
    if (!res.ok) {
      throw new Error(`Failed to fetch job: HTTP ${res.status}`);
    }
    return res.json();
  }

  static subscribeJobEvents(
    jobId: string,
    callbacks: SseJobCallbacks,
    options?: import('../types/api').SubscribeJobEventsOptions
  ): () => void {
    let url = `${API_BASE}/jobs/${encodeURIComponent(jobId)}/events`;
    if (options?.lastEventId !== undefined && options.lastEventId !== null && options.lastEventId !== '') {
      url += `?cursor=${encodeURIComponent(String(options.lastEventId))}`;
    }
    const eventSource = new EventSource(url);

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

    eventSource.addEventListener('done', (e: MessageEvent) => {
      try {
        if (callbacks.onDone) {
          callbacks.onDone(JSON.parse(e.data));
        }
      } catch (err) {
        console.error('[SSE] Failed to parse done payload', err);
        callbacks.onError?.('Failed to parse completion data');
      } finally {
        eventSource.close();
      }
    });

    eventSource.addEventListener('cancelled', (e: MessageEvent) => {
      let reason = 'Job was cancelled';
      try {
        const data = JSON.parse(e.data);
        if (data && typeof data.reason === 'string' && data.reason) {
          reason = data.reason;
        }
      } catch {
        // keep default reason
      }
      try {
        callbacks.onCancelled?.(reason);
      } finally {
        eventSource.close();
      }
    });

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
