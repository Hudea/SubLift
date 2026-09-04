import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import type { SystemInfoDTO, OcrEngineName, WorkspaceConfigDTO } from '../types/api';

export const useSystemStore = defineStore('system', () => {
  const isReady = ref(false);
  const isLoading = ref(false);
  const error = ref<string | null>(null);
  const systemInfo = ref<SystemInfoDTO | null>(null);
  const workspace = ref<WorkspaceConfigDTO>({
    configured: false,
    media_dir: '',
    cache_dir: '',
    video_count: 0,
    locked: false,
  });

  const availableEngines = computed(() => {
    if (!systemInfo.value) return [];
    return systemInfo.value.engines.filter((e) => e.available);
  });

  const preferredEngine = computed<OcrEngineName>(() => {
    const list = availableEngines.value.map((e) => e.name);
    if (list.includes('vision')) return 'vision';
    if (list.includes('paddle')) return 'paddle';
    return 'mock';
  });

  const isFfmpegReady = computed(() => {
    return systemInfo.value?.ffmpeg.available ?? false;
  });

  const serverVersion = computed(() => {
    return systemInfo.value?.version ?? 'v0.1.0';
  });

  const isWorkspaceConfigured = computed(() => {
    return workspace.value.configured && !!workspace.value.media_dir;
  });

  const isWorkspaceLocked = computed(() => {
    return workspace.value.locked === true;
  });

  const currentMediaDir = computed(() => {
    return workspace.value.media_dir;
  });

  async function fetchWorkspaceConfig() {
    try {
      const data = await SubLiftApiClient.getWorkspaceConfig();
      workspace.value = data;
    } catch {
      // ignore
    }
  }

  async function setWorkspace(dir: string): Promise<{ success: boolean; error?: string }> {
    if (isWorkspaceLocked.value) {
      return { success: false, error: '媒体工作区由服务启动配置锁定，不能通过界面修改' };
    }
    try {
      const data = await SubLiftApiClient.setWorkspaceConfig(dir);
      workspace.value = data;
      return { success: true };
    } catch (err: any) {
      return { success: false, error: err.message || '无法设置工作区目录' };
    }
  }

  async function clearWorkspace() {
    if (isWorkspaceLocked.value) {
      return;
    }
    try {
      const data = await SubLiftApiClient.clearWorkspaceConfig();
      workspace.value = data;
    } catch {
      // ignore
    }
  }

  async function fetchSystemInfo() {
    isLoading.value = true;
    error.value = null;
    try {
      const [data, ws] = await Promise.all([
        SubLiftApiClient.getSystemInfo(),
        SubLiftApiClient.getWorkspaceConfig().catch(() => ({
          configured: false,
          media_dir: '',
          cache_dir: '',
          video_count: 0,
          locked: false,
        })),
      ]);
      systemInfo.value = data;
      workspace.value = ws;
      isReady.value = true;
    } catch (err: any) {
      error.value = err.message || '无法连接到 SubLift 服务端';
      isReady.value = false;
    } finally {
      isLoading.value = false;
    }
  }

  return {
    isReady,
    isLoading,
    error,
    systemInfo,
    workspace,
    isWorkspaceConfigured,
    isWorkspaceLocked,
    currentMediaDir,
    availableEngines,
    preferredEngine,
    isFfmpegReady,
    serverVersion,
    fetchSystemInfo,
    fetchWorkspaceConfig,
    setWorkspace,
    clearWorkspace,
  };
});
