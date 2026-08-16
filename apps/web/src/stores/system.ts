import { defineStore } from 'pinia';
import { ref, computed } from 'vue';
import { SubLiftApiClient } from '../api/client';
import type { SystemInfoDTO, OcrEngineName } from '../types/api';

export const useSystemStore = defineStore('system', () => {
  const isReady = ref(false);
  const isLoading = ref(false);
  const error = ref<string | null>(null);
  const systemInfo = ref<SystemInfoDTO | null>(null);

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

  async function fetchSystemInfo() {
    isLoading.value = true;
    error.value = null;
    try {
      const data = await SubLiftApiClient.getSystemInfo();
      systemInfo.value = data;
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
    availableEngines,
    preferredEngine,
    isFfmpegReady,
    serverVersion,
    fetchSystemInfo,
  };
});
