import { parseIpcError } from './bridge';
import type {
  ProviderConnectionTestDto,
  ProviderProfileMaskedDto,
  ProviderUpsertDto,
  WorkbenchStore,
} from './types';

export type StoreSet = (
  partial:
    | Partial<WorkbenchStore>
    | ((state: WorkbenchStore) => Partial<WorkbenchStore>)
) => void;
export type StoreGet = () => WorkbenchStore;

export interface ProviderSliceHelpers {
  scheduleDraftSave?: (taskId: string) => void;
}

export function resolveDefaultProviderId(
  providers: readonly ProviderProfileMaskedDto[]
): string | null {
  const def = providers.find((p) => p.is_default);
  if (def) {
    return def.id;
  }
  return providers[0]?.id ?? null;
}

export function createProviderActions(
  set: StoreSet,
  get: StoreGet,
  helpers?: ProviderSliceHelpers
) {
  const syncTasksProviderList = (
    list: ProviderProfileMaskedDto[],
    options: {
      prevDefaultId: string | null;
      forcedDefaultId?: string | null;
      deletedId?: string;
      extraState?: Partial<WorkbenchStore>;
    }
  ) => {
    const defaultId = resolveDefaultProviderId(list);
    const validIds = new Set(list.map((p) => p.id));
    const changedIds = new Set<string>();

    set((state) => ({
      ...options.extraState,
      providers: list,
      tasks: state.tasks.map((task) => {
        if (task.status === 'extracting' || task.isReRecognizing) {
          return task;
        }
        let nextProviderId = task.providerId;
        if (options.forcedDefaultId) {
          nextProviderId = options.forcedDefaultId;
        } else if (
          (options.deletedId !== undefined && task.providerId === options.deletedId) ||
          task.providerId === null ||
          task.providerId === options.prevDefaultId ||
          !validIds.has(task.providerId)
        ) {
          nextProviderId = defaultId;
        }
        if (nextProviderId !== task.providerId) {
          changedIds.add(task.id);
          return { ...task, providerId: nextProviderId };
        }
        return task;
      }),
    }));

    for (const tid of changedIds) {
      helpers?.scheduleDraftSave?.(tid);
    }
  };

  return {
    async loadProviders(): Promise<void> {
      set({ providersLoading: true, providersError: null });
      try {
        const prevDefaultId = resolveDefaultProviderId(get().providers);
        const list = await get().bridge.listProviders();
        syncTasksProviderList(list, {
          prevDefaultId,
          extraState: { providersLoading: false },
        });
      } catch (err) {
        const parsed = parseIpcError(err);
        set({
          providersLoading: false,
          providersError: parsed.message,
        });
      }
    },

    async saveProvider(
      profile: ProviderUpsertDto
    ): Promise<ProviderProfileMaskedDto | null> {
      set({ providersError: null });
      try {
        const prevDefaultId = resolveDefaultProviderId(get().providers);
        const saved = await get().bridge.saveProvider(profile);
        const list = await get().bridge.listProviders();
        syncTasksProviderList(list, {
          prevDefaultId,
          forcedDefaultId: saved.is_default ? saved.id : null,
        });
        get().pushToast({
          severity: 'success',
          title: `已保存 VLM 配置「${saved.name}」`,
        });
        return saved;
      } catch (err) {
        const parsed = parseIpcError(err);
        set({ providersError: parsed.message });
        get().pushToast({
          severity: 'error',
          title: '保存 VLM 配置失败',
          reason: parsed.message,
          nextStep: '请检查 Base URL、模型名称与并发参数后重试',
        });
        return null;
      }
    },

    async testProviderConnection(
      profile: ProviderUpsertDto
    ): Promise<
      | { ok: true; result: ProviderConnectionTestDto }
      | { ok: false; error: string }
    > {
      try {
        const result = await get().bridge.testProviderConnection(profile);
        return { ok: true, result };
      } catch (err) {
        const parsed = parseIpcError(err);
        return { ok: false, error: parsed.message };
      }
    },

    async deleteProvider(id: string): Promise<boolean> {
      set({ providersError: null });
      try {
        const prevDefaultId = resolveDefaultProviderId(get().providers);
        await get().bridge.deleteProvider(id);
        const list = await get().bridge.listProviders();
        syncTasksProviderList(list, {
          prevDefaultId,
          deletedId: id,
        });
        get().pushToast({
          severity: 'info',
          title: '已删除 VLM 配置',
        });
        return true;
      } catch (err) {
        const parsed = parseIpcError(err);
        set({ providersError: parsed.message });
        get().pushToast({
          severity: 'error',
          title: '删除 VLM 配置失败',
          reason: parsed.message,
        });
        return false;
      }
    },

    async importProvidersFromEnv(
      envPath?: string
    ): Promise<ProviderProfileMaskedDto[]> {
      set({ providersImporting: true, providersError: null });
      try {
        const prevCount = get().providers.length;
        const prevDefaultId = resolveDefaultProviderId(get().providers);
        const list = await get().bridge.importProvidersFromEnv(envPath);
        syncTasksProviderList(list, {
          prevDefaultId,
          extraState: { providersImporting: false },
        });
        const added = Math.max(0, list.length - prevCount);
        get().pushToast({
          severity: list.length > 0 ? 'success' : 'info',
          title:
            list.length > 0
              ? `已从 .env 同步 ${list.length} 个 VLM 配置${added > 0 ? `（新增 ${added} 个）` : ''}`
              : '未在 .env 中检测到可用的 VLM 配置',
          nextStep: list.length === 0 ? '可手动点击「+ 新建配置」填写服务参数' : undefined,
        });
        return list;
      } catch (err) {
        const parsed = parseIpcError(err);
        set({
          providersImporting: false,
          providersError: parsed.message,
        });
        get().pushToast({
          severity: 'error',
          title: '从 .env 导入失败',
          reason: parsed.message,
          nextStep: '请确认工作区 .env 文件可读或手动新建配置',
        });
        return get().providers;
      }
    },
  };
}
