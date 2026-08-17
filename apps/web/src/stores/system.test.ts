import { describe, it, expect, beforeEach, vi } from 'vitest';
import { setActivePinia, createPinia } from 'pinia';
import { useSystemStore } from './system';
import { SubLiftApiClient } from '../api/client';

describe('System Store Workspace Management (Feature 12508)', () => {
  beforeEach(() => {
    setActivePinia(createPinia());
    vi.restoreAllMocks();
  });

  it('初始状态下未配置工作区', () => {
    const store = useSystemStore();
    expect(store.isWorkspaceConfigured).toBe(false);
    expect(store.currentMediaDir).toBe('');
  });

  it('fetchWorkspaceConfig 成功拉取工作区配置', async () => {
    const store = useSystemStore();
    vi.spyOn(SubLiftApiClient, 'getWorkspaceConfig').mockResolvedValue({
      configured: true,
      media_dir: '/Volumes/Data/videos',
      cache_dir: '/Volumes/Data/videos/.sublift_cache',
      video_count: 5,
    });

    await store.fetchWorkspaceConfig();

    expect(store.isWorkspaceConfigured).toBe(true);
    expect(store.currentMediaDir).toBe('/Volumes/Data/videos');
    expect(store.workspace.video_count).toBe(5);
  });

  it('setWorkspace 成功更新并持久化工作区', async () => {
    const store = useSystemStore();
    vi.spyOn(SubLiftApiClient, 'setWorkspaceConfig').mockResolvedValue({
      configured: true,
      media_dir: '/Users/test/Movies',
      cache_dir: '/Users/test/Movies/.sublift_cache',
      video_count: 2,
    });

    const res = await store.setWorkspace('/Users/test/Movies');

    expect(res.success).toBe(true);
    expect(store.isWorkspaceConfigured).toBe(true);
    expect(store.currentMediaDir).toBe('/Users/test/Movies');
  });

  it('setWorkspace 失败时返回错误并保持未配置', async () => {
    const store = useSystemStore();
    vi.spyOn(SubLiftApiClient, 'setWorkspaceConfig').mockRejectedValue(new Error('目录不存在'));

    const res = await store.setWorkspace('/invalid/path');

    expect(res.success).toBe(false);
    expect(res.error).toBe('目录不存在');
    expect(store.isWorkspaceConfigured).toBe(false);
  });

  it('clearWorkspace 清空当前工作区状态', async () => {
    const store = useSystemStore();
    store.workspace = {
      configured: true,
      media_dir: '/Users/test/Movies',
      cache_dir: '/Users/test/Movies/.sublift_cache',
      video_count: 2,
    };

    vi.spyOn(SubLiftApiClient, 'clearWorkspaceConfig').mockResolvedValue({
      configured: false,
      media_dir: '',
      cache_dir: '/tmp/sublift_cache',
      video_count: 0,
    });

    await store.clearWorkspace();

    expect(store.isWorkspaceConfigured).toBe(false);
    expect(store.currentMediaDir).toBe('');
  });
});
