import { useEffect, useRef, useState } from 'react';
import {
  CheckCircle2,
  ChevronDown,
  ChevronRight,
  FileCode2,
  KeyRound,
  Loader2,
  PlugZap,
  Plus,
  ShieldCheck,
  Trash2,
  XCircle,
} from 'lucide-react';
import type {
  ApiKeyUpdate,
  CustomHeaderDto,
  ProviderConnectionTestDto,
  ProviderProfileMaskedDto,
  ProviderType,
  ThinkingMode,
} from '../../core/types';
import { selectEffectiveProvider, useWorkbenchStore } from '../../store';
import { Button } from '../components/ui/button';
import { Checkbox } from '../components/ui/checkbox';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '../components/ui/dialog';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '../components/ui/select';
import { cn } from '../lib/utils';

const PRESET_DEFAULTS: Record<
  ProviderType,
  {
    label: string;
    baseUrl: string;
    model: string;
    namePlaceholder: string;
    baseUrlPlaceholder: string;
    modelPlaceholder: string;
  }
> = {
  openai: {
    label: 'OpenAI 兼容',
    baseUrl: '',
    model: '',
    namePlaceholder: '例如：OpenAI 兼容服务',
    baseUrlPlaceholder: 'https://api.openai.com/v1',
    modelPlaceholder: 'gpt-4o-mini',
  },
  minimax: {
    label: 'MiniMax',
    baseUrl: 'https://api.minimax.cn/v1',
    model: 'MiniMax-M3.1-Flash',
    namePlaceholder: '例如：MiniMax 主力',
    baseUrlPlaceholder: 'https://api.minimax.cn/v1',
    modelPlaceholder: 'MiniMax-M3.1-Flash',
  },
  nvidia: {
    label: 'NVIDIA NIM',
    baseUrl: 'https://integrate.api.nvidia.com/v1',
    model: 'nvidia/nemotron-3.5-super-vl-120b-a12b',
    namePlaceholder: '例如：NVIDIA NIM',
    baseUrlPlaceholder: 'https://integrate.api.nvidia.com/v1',
    modelPlaceholder: 'nvidia/nemotron-3.5-super-vl-120b-a12b',
  },
};

function isPresetDefaultUrl(url: string): boolean {
  const trimmed = url.trim();
  return (
    trimmed === '' ||
    trimmed === PRESET_DEFAULTS.minimax.baseUrl ||
    trimmed === PRESET_DEFAULTS.nvidia.baseUrl
  );
}

function isPresetDefaultModel(model: string): boolean {
  const trimmed = model.trim();
  return (
    trimmed === '' ||
    trimmed === PRESET_DEFAULTS.minimax.model ||
    trimmed === PRESET_DEFAULTS.nvidia.model
  );
}

const HEADER_NAME_RE = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const RESERVED_HEADER_NAMES = new Set([
  'host',
  'content-length',
  'transfer-encoding',
  'connection',
  'upgrade',
  'te',
  'trailer',
]);
const MAX_CUSTOM_HEADERS = 32;

function hasForbiddenHeaderChars(val: string): boolean {
  for (let i = 0; i < val.length; i++) {
    const code = val.charCodeAt(i);
    if (code === 0 || code === 0x0a || code === 0x0d) {
      return true;
    }
    if ((code < 0x20 && code !== 0x09) || code === 0x7f) {
      return true;
    }
  }
  return false;
}

/**
 * 解析剪贴板中的请求头文本（支持 JSON 对象与单行/多行 Key: Value）。
 * 若输入为 http/https URL 或不符合请求头格式，返回 null 交由浏览器默认粘贴。
 */
export function parsePastedCustomHeaders(raw: string): CustomHeaderDto[] | null {
  const trimmed = raw.trim();
  if (!trimmed || /^https?:\/\//i.test(trimmed)) {
    return null;
  }

  if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
    try {
      const parsed: unknown = JSON.parse(trimmed);
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        const result: CustomHeaderDto[] = [];
        for (const [k, v] of Object.entries(parsed)) {
          const name = k.trim();
          if (
            !HEADER_NAME_RE.test(name) ||
            (typeof v !== 'string' &&
              typeof v !== 'number' &&
              typeof v !== 'boolean')
          ) {
            return null;
          }
          const value = String(v).trim();
          if (hasForbiddenHeaderChars(value)) {
            return null;
          }
          result.push({ name, value });
        }
        return result.length > 0 ? result : null;
      }
    } catch {
      return null;
    }
    return null;
  }

  const lines = raw.split(/\r?\n/);
  const result: CustomHeaderDto[] = [];
  for (const rawLine of lines) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) {
      continue;
    }
    if (/^https?:\/\//i.test(line)) {
      return null;
    }
    const colonIdx = line.indexOf(':');
    if (colonIdx <= 0) {
      return null;
    }
    const name = line.slice(0, colonIdx).trim();
    const value = line.slice(colonIdx + 1).trim();
    if (!HEADER_NAME_RE.test(name) || hasForbiddenHeaderChars(value)) {
      return null;
    }
    result.push({ name, value });
  }
  return result.length > 0 ? result : null;
}

function validateNormalizedHeaders(
  items: CustomHeaderDto[]
): { ok: true; headers: CustomHeaderDto[] } | { ok: false; error: string } {
  const cleaned: CustomHeaderDto[] = [];
  for (const row of items) {
    const hName = row.name.trim();
    const hValue = row.value.trim();
    if (!hName && !hValue) {
      continue;
    }
    if (!hName) {
      return { ok: false, error: '存在未填写名称的自定义请求头' };
    }
    if (!HEADER_NAME_RE.test(hName)) {
      return {
        ok: false,
        error: `自定义请求头名称「${hName}」包含非法字符`,
      };
    }
    if (RESERVED_HEADER_NAMES.has(hName.toLowerCase())) {
      return {
        ok: false,
        error: `请求头「${hName}」属于底层传输保留头，不允许覆盖`,
      };
    }
    if (hasForbiddenHeaderChars(hValue)) {
      return {
        ok: false,
        error: `自定义请求头「${hName}」的值包含非法控制字符`,
      };
    }
    cleaned.push({ name: hName, value: hValue });
  }
  if (cleaned.length > MAX_CUSTOM_HEADERS) {
    return {
      ok: false,
      error: `自定义请求头最多支持 ${MAX_CUSTOM_HEADERS} 项`,
    };
  }
  return { ok: true, headers: cleaned };
}

export function formatCustomHeadersAsText(headers: CustomHeaderDto[]): string {
  const nonBlank = headers
    .filter((h) => h.name.trim().length > 0 || h.value.trim().length > 0)
    .map((h) => ({ name: h.name.trim(), value: h.value.trim() }));
  if (nonBlank.length === 0) {
    return '';
  }
  const seenLower = new Set<string>();
  let canFormatAsJson = true;
  for (const row of nonBlank) {
    const lower = row.name.toLowerCase();
    if (!row.name || !HEADER_NAME_RE.test(row.name) || seenLower.has(lower)) {
      canFormatAsJson = false;
      break;
    }
    seenLower.add(lower);
  }
  if (canFormatAsJson) {
    const obj: Record<string, string> = {};
    for (const row of nonBlank) {
      obj[row.name] = row.value;
    }
    return JSON.stringify(obj, null, 2);
  }
  return nonBlank.map((row) => `${row.name}: ${row.value}`).join('\n');
}

export function parseBulkCustomHeadersText(
  raw: string
): { ok: true; headers: CustomHeaderDto[] } | { ok: false; error: string } {
  const trimmed = raw.trim();
  if (!trimmed) {
    return { ok: true, headers: [] };
  }

  if (trimmed.startsWith('{') || trimmed.startsWith('[')) {
    let parsed: unknown;
    try {
      parsed = JSON.parse(trimmed);
    } catch {
      return { ok: false, error: 'JSON 格式无效，请检查括号与引号配对' };
    }
    if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
      return {
        ok: false,
        error: '自定义请求头 JSON 必须是键值对象（例如 {"X-Title": "SubLift"}）',
      };
    }
    const items: CustomHeaderDto[] = [];
    for (const [k, v] of Object.entries(parsed)) {
      const name = k.trim();
      if (!name) {
        return { ok: false, error: 'JSON 中存在空的请求头名称' };
      }
      if (
        typeof v !== 'string' &&
        typeof v !== 'number' &&
        typeof v !== 'boolean'
      ) {
        return {
          ok: false,
          error: `请求头「${name}」的值必须为字符串、数字或布尔值`,
        };
      }
      items.push({ name, value: String(v).trim() });
    }
    return validateNormalizedHeaders(items);
  }

  const lines = raw.split(/\r?\n/);
  const items: CustomHeaderDto[] = [];
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line || line.startsWith('#')) {
      continue;
    }
    if (/^https?:\/\//i.test(line)) {
      return {
        ok: false,
        error: '多行文本中请填写「名称: 值」格式，不能直接填写 URL',
      };
    }
    const colonIdx = line.indexOf(':');
    if (colonIdx <= 0) {
      return {
        ok: false,
        error: `第 ${i + 1} 行缺少「名称: 值」中的冒号`,
      };
    }
    items.push({
      name: line.slice(0, colonIdx).trim(),
      value: line.slice(colonIdx + 1).trim(),
    });
  }
  return validateNormalizedHeaders(items);
}

interface FormState {
  id: string | null;
  name: string;
  provider_type: ProviderType;
  base_url: string;
  model: string;
  thinking_mode: ThinkingMode;
  concurrency: number;
  is_default: boolean;
  rawApiKey: string;
  clearKey: boolean;
  hasExistingKey: boolean;
  maskedKey: string;
  custom_headers: CustomHeaderDto[];
}

function fromProfile(p: ProviderProfileMaskedDto): FormState {
  return {
    id: p.id,
    name: p.name,
    provider_type: p.provider_type,
    base_url: p.base_url,
    model: p.model,
    thinking_mode: p.thinking_mode,
    concurrency: p.concurrency,
    is_default: p.is_default,
    rawApiKey: '',
    clearKey: false,
    hasExistingKey: p.has_api_key,
    maskedKey: p.masked_api_key,
    custom_headers: (p.custom_headers ?? []).map((h) => ({ ...h })),
  };
}

function createBlankForm(isDefault: boolean): FormState {
  return {
    id: null,
    name: '',
    provider_type: 'openai',
    base_url: PRESET_DEFAULTS.openai.baseUrl,
    model: PRESET_DEFAULTS.openai.model,
    thinking_mode: 'auto',
    concurrency: 6,
    is_default: isDefault,
    rawApiKey: '',
    clearKey: false,
    hasExistingKey: false,
    maskedKey: '',
    custom_headers: [],
  };
}

function resolvePreferredProfile(
  list: readonly ProviderProfileMaskedDto[]
): ProviderProfileMaskedDto | null {
  return list.find((p) => p.is_default) ?? list[0] ?? null;
}

export function VlmSettingsModal() {
  const open = useWorkbenchStore((s) => s.vlmSettingsOpen);
  const providers = useWorkbenchStore((s) => s.providers);
  const providersImporting = useWorkbenchStore((s) => s.providersImporting);
  const setVlmSettingsOpen = useWorkbenchStore((s) => s.setVlmSettingsOpen);
  const saveProvider = useWorkbenchStore((s) => s.saveProvider);
  const testProviderConnection = useWorkbenchStore(
    (s) => s.testProviderConnection
  );
  const deleteProvider = useWorkbenchStore((s) => s.deleteProvider);
  const importProvidersFromEnv = useWorkbenchStore(
    (s) => s.importProvidersFromEnv
  );

  const [form, setForm] = useState<FormState>(() => {
    const preferred = resolvePreferredProfile(providers);
    return preferred ? fromProfile(preferred) : createBlankForm(true);
  });
  const [headersExpanded, setHeadersExpanded] = useState<boolean>(
    () => form.custom_headers.length > 0
  );
  const [headerEditMode, setHeaderEditMode] = useState<'kv' | 'text'>('kv');
  const [rawHeadersText, setRawHeadersText] = useState<string>(() =>
    formatCustomHeadersAsText(form.custom_headers)
  );
  const [validationError, setValidationError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [testingConnection, setTestingConnection] = useState(false);
  const [connectionTestResult, setConnectionTestResult] = useState<
    | { ok: true; data: ProviderConnectionTestDto }
    | { ok: false; message: string }
    | null
  >(null);
  const prevOpenRef = useRef(false);

  const applyProfileForm = (
    profile: ProviderProfileMaskedDto | null,
    defaultIfBlank: boolean
  ) => {
    const next = profile ? fromProfile(profile) : createBlankForm(defaultIfBlank);
    setForm(next);
    setHeadersExpanded(next.custom_headers.length > 0);
    setHeaderEditMode('kv');
    setRawHeadersText(formatCustomHeadersAsText(next.custom_headers));
    setConnectionTestResult(null);
  };

  useEffect(() => {
    const justOpened = open && !prevOpenRef.current;
    prevOpenRef.current = open;
    if (!open) {
      return;
    }
    setValidationError(null);
    if (justOpened) {
      const target =
        selectEffectiveProvider(useWorkbenchStore.getState()) ??
        resolvePreferredProfile(providers);
      applyProfileForm(target, true);
      return;
    }
    if (form.id) {
      const updated = providers.find((p) => p.id === form.id);
      if (updated) {
        setForm(fromProfile(updated));
        setRawHeadersText(formatCustomHeadersAsText(updated.custom_headers ?? []));
        return;
      }
    }
    if (providers.length > 0 && form.id === null && form.name === '') {
      const preferred = resolvePreferredProfile(providers);
      if (preferred) {
        applyProfileForm(preferred, true);
      }
    }
  }, [open, providers]);

  const handleSelectProfile = (p: ProviderProfileMaskedDto) => {
    setValidationError(null);
    applyProfileForm(p, true);
  };

  const handleNewProfile = () => {
    setValidationError(null);
    applyProfileForm(null, providers.length === 0);
  };

  const handleProviderTypeChange = (nextType: ProviderType) => {
    setConnectionTestResult(null);
    setForm((prev) => {
      const preset = PRESET_DEFAULTS[nextType];
      return {
        ...prev,
        provider_type: nextType,
        base_url: isPresetDefaultUrl(prev.base_url)
          ? preset.baseUrl
          : prev.base_url,
        model: isPresetDefaultModel(prev.model) ? preset.model : prev.model,
      };
    });
  };

  const handleToggleHeaders = () => {
    setHeadersExpanded((prev) => {
      const next = !prev;
      if (next && headerEditMode === 'kv') {
        setForm((cur) =>
          cur.custom_headers.length === 0
            ? { ...cur, custom_headers: [{ name: '', value: '' }] }
            : cur
        );
      }
      return next;
    });
  };

  const handleSwitchHeaderMode = (nextMode: 'kv' | 'text') => {
    if (nextMode === headerEditMode) {
      return;
    }
    if (nextMode === 'text') {
      setValidationError(null);
      setRawHeadersText(formatCustomHeadersAsText(form.custom_headers));
      setHeaderEditMode('text');
      return;
    }
    const parsed = parseBulkCustomHeadersText(rawHeadersText);
    if (!parsed.ok) {
      setValidationError(parsed.error);
      return;
    }
    setValidationError(null);
    setForm((prev) => ({
      ...prev,
      custom_headers:
        parsed.headers.length > 0 ? parsed.headers : [{ name: '', value: '' }],
    }));
    setHeaderEditMode('kv');
  };

  const handleHeaderFieldChange = (
    idx: number,
    field: 'name' | 'value',
    val: string
  ) => {
    setConnectionTestResult(null);
    setForm((prev) => ({
      ...prev,
      custom_headers: prev.custom_headers.map((row, i) =>
        i === idx ? { ...row, [field]: val } : row
      ),
    }));
  };

  const handleAddHeaderRow = () => {
    setConnectionTestResult(null);
    setForm((prev) => {
      if (prev.custom_headers.length >= MAX_CUSTOM_HEADERS) {
        return prev;
      }
      return {
        ...prev,
        custom_headers: [...prev.custom_headers, { name: '', value: '' }],
      };
    });
  };

  const handleRemoveHeaderRow = (idx: number) => {
    setConnectionTestResult(null);
    setForm((prev) => ({
      ...prev,
      custom_headers: prev.custom_headers.filter((_, i) => i !== idx),
    }));
  };

  const handleHeaderPaste = (
    e: React.ClipboardEvent<HTMLInputElement>,
    idx: number,
    field: 'name' | 'value'
  ) => {
    const text = e.clipboardData?.getData('text') ?? '';
    if (!text) {
      return;
    }
    const parsed = parsePastedCustomHeaders(text);
    if (!parsed) {
      return;
    }
    if (field === 'value') {
      const currentName = form.custom_headers[idx]?.name.trim() ?? '';
      const looksStructured = parsed.length > 1 || text.trim().startsWith('{');
      if (currentName.length > 0 || !looksStructured) {
        return;
      }
    }
    e.preventDefault();
    setConnectionTestResult(null);
    setForm((prev) => {
      const before = prev.custom_headers.slice(0, idx);
      const after = prev.custom_headers.slice(idx + 1);
      const merged = [...before, ...parsed, ...after].slice(
        0,
        MAX_CUSTOM_HEADERS
      );
      return { ...prev, custom_headers: merged };
    });
  };

  const buildApiKeyUpdate = (): ApiKeyUpdate => {
    if (form.clearKey) {
      return { mode: 'clear' };
    }
    if (form.rawApiKey.trim().length > 0) {
      return { mode: 'replace', value: form.rawApiKey.trim() };
    }
    return { mode: 'keep' };
  };

  const resolveValidatedHeaders = (): CustomHeaderDto[] | null => {
    if (headerEditMode === 'text') {
      const parsed = parseBulkCustomHeadersText(rawHeadersText);
      if (!parsed.ok) {
        setValidationError(parsed.error);
        return null;
      }
      setForm((prev) => ({
        ...prev,
        custom_headers: parsed.headers,
      }));
      return parsed.headers;
    }
    const validated = validateNormalizedHeaders(form.custom_headers);
    if (!validated.ok) {
      setValidationError(validated.error);
      return null;
    }
    return validated.headers;
  };

  const handleTestConnection = async () => {
    const baseUrl = form.base_url.trim();
    const model = form.model.trim();
    if (!/^https?:\/\/.+/.test(baseUrl)) {
      setValidationError('API Base URL 必须以 http:// 或 https:// 开头');
      return;
    }
    if (!model) {
      setValidationError('请填写模型标识 (model)');
      return;
    }
    const cleanedHeaders = resolveValidatedHeaders();
    if (!cleanedHeaders) {
      return;
    }

    setValidationError(null);
    setConnectionTestResult(null);
    setTestingConnection(true);
    const res = await testProviderConnection({
      id: form.id,
      name: form.name.trim(),
      provider_type: form.provider_type,
      base_url: baseUrl,
      model,
      thinking_mode: form.thinking_mode,
      concurrency: form.concurrency,
      is_default: form.is_default,
      api_key_update: buildApiKeyUpdate(),
      custom_headers: cleanedHeaders,
    });
    setTestingConnection(false);
    if (res.ok) {
      setConnectionTestResult({ ok: true, data: res.result });
    } else {
      setConnectionTestResult({ ok: false, message: res.error });
    }
  };

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    const name = form.name.trim();
    const baseUrl = form.base_url.trim();
    const model = form.model.trim();

    if (!name) {
      setValidationError('请填写配置名称');
      return;
    }
    if (!/^https?:\/\/.+/.test(baseUrl)) {
      setValidationError('API Base URL 必须以 http:// 或 https:// 开头');
      return;
    }
    if (!model) {
      setValidationError('请填写模型标识 (model)');
      return;
    }
    if (
      !Number.isInteger(form.concurrency) ||
      form.concurrency < 1 ||
      form.concurrency > 16
    ) {
      setValidationError('并发度必须在 1 到 16 之间');
      return;
    }

    const cleanedHeaders = resolveValidatedHeaders();
    if (!cleanedHeaders) {
      return;
    }

    setValidationError(null);
    setSaving(true);
    const saved = await saveProvider({
      id: form.id,
      name,
      provider_type: form.provider_type,
      base_url: baseUrl,
      model,
      thinking_mode: form.thinking_mode,
      concurrency: form.concurrency,
      is_default: form.is_default,
      api_key_update: buildApiKeyUpdate(),
      custom_headers: cleanedHeaders,
    });
    setSaving(false);
    if (saved) {
      setForm(fromProfile(saved));
      setRawHeadersText(formatCustomHeadersAsText(saved.custom_headers ?? []));
    }
  };

  const handleDelete = async (id: string) => {
    const ok = await deleteProvider(id);
    if (ok) {
      const remaining = useWorkbenchStore.getState().providers;
      const preferred = resolvePreferredProfile(remaining);
      applyProfileForm(preferred, true);
    }
  };

  const handleImportEnv = async () => {
    const list = await importProvidersFromEnv();
    const preferred = resolvePreferredProfile(list);
    if (preferred) {
      applyProfileForm(preferred, true);
    }
  };

  const presetMeta = PRESET_DEFAULTS[form.provider_type];
  const activeHeaderCount =
    headerEditMode === 'text'
      ? (() => {
          const parsed = parseBulkCustomHeadersText(rawHeadersText);
          return parsed.ok
            ? parsed.headers.length
            : form.custom_headers.filter(
                (h) => h.name.trim().length > 0 || h.value.trim().length > 0
              ).length;
        })()
      : form.custom_headers.filter(
          (h) => h.name.trim().length > 0 || h.value.trim().length > 0
        ).length;

  return (
    <Dialog open={open} onOpenChange={setVlmSettingsOpen}>
      <DialogContent className="flex max-h-[85vh] max-w-[720px] flex-col overflow-hidden p-0">
        <div className="border-b border-[var(--border-subtle)] px-4 py-3">
          <DialogHeader>
            <DialogTitle>VLM 多配置管理</DialogTitle>
            <DialogDescription>
              支持兼容 OpenAI{' '}
              <code className="rounded bg-[var(--bg-elevated)] px-1 py-0.5 font-mono text-[11px] text-[var(--text-primary)]">
                /chat/completions
              </code>{' '}
              的视觉模型端点
            </DialogDescription>
          </DialogHeader>
        </div>

        <div className="grid min-h-0 flex-1 grid-cols-[230px_1fr] grid-rows-[minmax(0,1fr)]">
          {/* 左侧档案列表 */}
          <div className="flex min-h-[420px] flex-col border-r border-[var(--border-subtle)] bg-[var(--bg-panel)] p-3">
            <div className="flex items-center gap-1.5">
              <Button
                size="xs"
                variant="primary"
                className="flex-1"
                onClick={handleNewProfile}
              >
                <Plus className="h-3.5 w-3.5" strokeWidth={1.75} />
                <span>新建配置</span>
              </Button>
              <Button
                size="xs"
                variant="outline"
                disabled={providersImporting}
                onClick={() => void handleImportEnv()}
              >
                <FileCode2 className="h-3.5 w-3.5" strokeWidth={1.75} />
                <span>{providersImporting ? '导入中' : '从 .env 导入'}</span>
              </Button>
            </div>

            <div className="mt-3 flex-1 space-y-1.5 overflow-y-auto">
              {providers.length === 0 ? (
                <p className="rounded-[var(--radius-md)] border border-dashed border-[var(--border-default)] p-3 text-center text-[11px] text-[var(--text-muted)]">
                  暂无 VLM 配置档案
                </p>
              ) : (
                providers.map((p) => {
                  const selected = form.id === p.id;
                  return (
                    <div
                      key={p.id}
                      role="button"
                      tabIndex={0}
                      onClick={() => handleSelectProfile(p)}
                      onKeyDown={(e) => {
                        if (e.key === 'Enter' || e.key === ' ') {
                          e.preventDefault();
                          handleSelectProfile(p);
                        }
                      }}
                      className={cn(
                        'flex cursor-pointer items-start justify-between gap-2 rounded-[var(--radius-md)] border p-2 text-left text-xs transition-colors',
                        selected
                          ? 'border-[var(--sl-highlight)]/70 bg-[var(--bg-active)]'
                          : 'border-[var(--border-subtle)] bg-[var(--bg-elevated)] hover:bg-[var(--bg-hover)]'
                      )}
                    >
                      <div className="min-w-0 flex-1">
                        <div className="flex items-center gap-1.5">
                          {p.is_default && (
                            <span
                              className="h-1.5 w-1.5 shrink-0 rounded-full bg-[var(--status-exported)]"
                              title="默认配置"
                            />
                          )}
                          <span className="truncate font-medium text-[var(--text-primary)]">
                            {p.name}
                          </span>
                        </div>
                        <div className="mt-1 flex items-center gap-1.5 text-[10px] text-[var(--text-secondary)]">
                          <span className="rounded bg-[var(--bg-panel)] px-1 py-0.5">
                            {PRESET_DEFAULTS[p.provider_type].label}
                          </span>
                          <span>
                            {p.has_api_key ? '已配置密钥' : '未填密钥'}
                          </span>
                        </div>
                      </div>
                      <button
                        type="button"
                        aria-label={`删除配置 ${p.name}`}
                        onClick={(e) => {
                          e.stopPropagation();
                          void handleDelete(p.id);
                        }}
                        className="rounded p-1 text-[var(--text-muted)] hover:bg-[var(--tint-failed)] hover:text-[var(--status-failed)]"
                      >
                        <Trash2 className="h-3.5 w-3.5" strokeWidth={1.75} />
                      </button>
                    </div>
                  );
                })
              )}
            </div>
          </div>

          {/* 右侧编辑表单 */}
          <form
            onSubmit={(e) => void handleSubmit(e)}
            className="flex min-h-0 flex-1 flex-col overflow-y-auto p-4"
          >
            <div className="grid grid-cols-2 gap-3 text-xs">
              <div className="space-y-1">
                <label
                  htmlFor="vlm-name"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  配置名称
                </label>
                <input
                  id="vlm-name"
                  type="text"
                  value={form.name}
                  onChange={(e) => setForm({ ...form, name: e.target.value })}
                  placeholder={presetMeta.namePlaceholder}
                  className="h-7 w-full rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                />
              </div>

              <div className="space-y-1">
                <label
                  htmlFor="vlm-provider-type"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  供应商预设
                </label>
                <Select
                  value={form.provider_type}
                  onValueChange={(v) =>
                    handleProviderTypeChange(v as ProviderType)
                  }
                >
                  <SelectTrigger id="vlm-provider-type" className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="openai">OpenAI 兼容</SelectItem>
                    <SelectItem value="minimax">MiniMax</SelectItem>
                    <SelectItem value="nvidia">NVIDIA NIM</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="col-span-2 space-y-1">
                <label
                  htmlFor="vlm-base-url"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  API Base URL
                </label>
                <input
                  id="vlm-base-url"
                  type="text"
                  value={form.base_url}
                  onChange={(e) => {
                    setConnectionTestResult(null);
                    setForm({ ...form, base_url: e.target.value });
                  }}
                  placeholder={presetMeta.baseUrlPlaceholder}
                  className="h-7 w-full rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 font-mono text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                />
              </div>

              <div className="col-span-2 space-y-1">
                <label
                  htmlFor="vlm-model"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  模型标识 (Model)
                </label>
                <input
                  id="vlm-model"
                  type="text"
                  value={form.model}
                  onChange={(e) => {
                    setConnectionTestResult(null);
                    setForm({ ...form, model: e.target.value });
                  }}
                  placeholder={presetMeta.modelPlaceholder}
                  className="h-7 w-full rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 font-mono text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                />
              </div>

              <div className="space-y-1">
                <label
                  htmlFor="vlm-thinking"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  思考模式 (Thinking Mode)
                </label>
                <Select
                  value={form.thinking_mode}
                  onValueChange={(v) => {
                    setConnectionTestResult(null);
                    setForm({ ...form, thinking_mode: v as ThinkingMode });
                  }}
                >
                  <SelectTrigger id="vlm-thinking" className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="auto">auto（自适应降级）</SelectItem>
                    <SelectItem value="none">none</SelectItem>
                    <SelectItem value="low">low</SelectItem>
                    <SelectItem value="minimal">minimal</SelectItem>
                    <SelectItem value="default">default</SelectItem>
                  </SelectContent>
                </Select>
              </div>

              <div className="space-y-1">
                <label
                  htmlFor="vlm-concurrency"
                  className="block text-[11px] font-medium text-[var(--text-secondary)]"
                >
                  最大并发度 (1–16)
                </label>
                <input
                  id="vlm-concurrency"
                  type="number"
                  min={1}
                  max={16}
                  value={form.concurrency}
                  onChange={(e) =>
                    setForm({
                      ...form,
                      concurrency: Number(e.target.value) || 1,
                    })
                  }
                  className="tabular-nums-mono h-7 w-full rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                />
              </div>

              {/* 只读掩码与防覆盖 API Key 输入 */}
              <div className="col-span-2 space-y-1.5">
                <div className="flex items-center justify-between">
                  <label
                    htmlFor="vlm-api-key"
                    className="block text-[11px] font-medium text-[var(--text-secondary)]"
                  >
                    API Key
                  </label>
                  {form.hasExistingKey && !form.clearKey && (
                    <span className="inline-flex items-center gap-1 rounded-[var(--radius-xs)] bg-[var(--bg-panel)] px-1.5 py-0.5 font-mono text-[10px] text-[var(--text-secondary)]">
                      <KeyRound className="h-3 w-3 text-[var(--status-exported)]" />
                      <span>当前已保存密钥: {form.maskedKey}</span>
                    </span>
                  )}
                </div>

                <div className="flex items-center gap-2">
                  <input
                    id="vlm-api-key"
                    type="password"
                    disabled={form.clearKey}
                    value={form.rawApiKey}
                    onChange={(e) => {
                      setConnectionTestResult(null);
                      setForm({ ...form, rawApiKey: e.target.value });
                    }}
                    placeholder={
                      form.clearKey
                        ? '将在保存后清除当前密钥'
                        : form.hasExistingKey
                          ? '输入新 API Key 以替换，留空则保留现有密钥'
                          : '输入 API Key'
                    }
                    className="h-7 flex-1 rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 text-xs text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none disabled:opacity-45"
                  />
                  {form.hasExistingKey && (
                    <Button
                      size="xs"
                      variant={form.clearKey ? 'danger' : 'outline'}
                      onClick={() => {
                        setConnectionTestResult(null);
                        setForm({
                          ...form,
                          clearKey: !form.clearKey,
                          rawApiKey: '',
                        });
                      }}
                    >
                      {form.clearKey ? '取消清除' : '清除密钥'}
                    </Button>
                  )}
                </div>
              </div>

              <div className="col-span-2 rounded-[var(--radius-md)] border border-[var(--border-subtle)] bg-[var(--bg-panel)]/60">
                <button
                  type="button"
                  aria-expanded={headersExpanded}
                  onClick={handleToggleHeaders}
                  className="flex w-full items-center justify-between px-3 py-2 text-left text-xs font-medium text-[var(--text-primary)] hover:bg-[var(--bg-hover)]"
                >
                  <span className="flex items-center gap-1.5">
                    {headersExpanded ? (
                      <ChevronDown className="h-3.5 w-3.5 text-[var(--text-secondary)]" />
                    ) : (
                      <ChevronRight className="h-3.5 w-3.5 text-[var(--text-secondary)]" />
                    )}
                    <span>自定义请求头（可选）</span>
                  </span>
                  {activeHeaderCount > 0 && (
                    <span className="rounded bg-[var(--bg-active)] px-1.5 py-0.5 text-[10px] text-[var(--sl-highlight)]">
                      已配置 {activeHeaderCount} 项
                    </span>
                  )}
                </button>

                {headersExpanded && (
                  <div className="space-y-2 border-t border-[var(--border-subtle)] p-3">
                    <div className="flex flex-wrap items-start justify-between gap-2">
                      <p className="min-w-0 flex-1 text-[11px] leading-relaxed text-[var(--text-muted)]">
                        用于需要附加 Header 的网关（如{' '}
                        <code className="rounded bg-[var(--bg-elevated)] px-1 font-mono text-[10px] text-[var(--text-secondary)]">
                          x-opencode-session
                        </code>
                        、
                        <code className="rounded bg-[var(--bg-elevated)] px-1 font-mono text-[10px] text-[var(--text-secondary)]">
                          User-Agent
                        </code>
                        、
                        <code className="rounded bg-[var(--bg-elevated)] px-1 font-mono text-[10px] text-[var(--text-secondary)]">
                          X-Title
                        </code>
                        ）。支持{' '}
                        <code className="rounded bg-[var(--bg-elevated)] px-1 font-mono text-[10px] text-[var(--text-secondary)]">
                          {'{{session_id}}'}
                        </code>{' '}
                        按任务自动生成 UUID。
                      </p>
                      <div
                        role="group"
                        aria-label="请求头编辑模式"
                        className="inline-flex shrink-0 rounded-[var(--radius-sm)] border border-[var(--border-subtle)] bg-[var(--bg-elevated)] p-0.5 text-[10px]"
                      >
                        <button
                          type="button"
                          onClick={() => handleSwitchHeaderMode('kv')}
                          className={cn(
                            'rounded-[var(--radius-xs)] px-2 py-0.5 font-medium transition-colors',
                            headerEditMode === 'kv'
                              ? 'bg-[var(--bg-active)] text-[var(--text-primary)]'
                              : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                          )}
                        >
                          键值表单
                        </button>
                        <button
                          type="button"
                          onClick={() => handleSwitchHeaderMode('text')}
                          className={cn(
                            'rounded-[var(--radius-xs)] px-2 py-0.5 font-medium transition-colors',
                            headerEditMode === 'text'
                              ? 'bg-[var(--bg-active)] text-[var(--text-primary)]'
                              : 'text-[var(--text-secondary)] hover:text-[var(--text-primary)]'
                          )}
                        >
                          JSON / 文本
                        </button>
                      </div>
                    </div>

                    {headerEditMode === 'kv' ? (
                      <>
                        <div className="space-y-1.5">
                          {form.custom_headers.map((header, idx) => (
                            <div key={idx} className="flex items-center gap-1.5">
                              <input
                                type="text"
                                aria-label={`请求头名称 ${idx + 1}`}
                                value={header.name}
                                onChange={(e) =>
                                  handleHeaderFieldChange(
                                    idx,
                                    'name',
                                    e.target.value
                                  )
                                }
                                onPaste={(e) =>
                                  handleHeaderPaste(e, idx, 'name')
                                }
                                placeholder="例如：x-opencode-session"
                                className="h-7 w-[42%] rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 font-mono text-[11px] text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                              />
                              <input
                                type="text"
                                aria-label={`请求头值 ${idx + 1}`}
                                value={header.value}
                                onChange={(e) =>
                                  handleHeaderFieldChange(
                                    idx,
                                    'value',
                                    e.target.value
                                  )
                                }
                                onPaste={(e) =>
                                  handleHeaderPaste(e, idx, 'value')
                                }
                                placeholder="例如：{{session_id}} 或具体值"
                                className="h-7 flex-1 rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] px-2 font-mono text-[11px] text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                              />
                              <button
                                type="button"
                                aria-label={`删除请求头 ${idx + 1}`}
                                onClick={() => handleRemoveHeaderRow(idx)}
                                className="rounded p-1.5 text-[var(--text-muted)] hover:bg-[var(--tint-failed)] hover:text-[var(--status-failed)]"
                              >
                                <Trash2
                                  className="h-3.5 w-3.5"
                                  strokeWidth={1.75}
                                />
                              </button>
                            </div>
                          ))}
                        </div>

                        <div className="flex items-center justify-between pt-0.5">
                          <Button
                            type="button"
                            size="xs"
                            variant="outline"
                            disabled={
                              form.custom_headers.length >= MAX_CUSTOM_HEADERS
                            }
                            onClick={handleAddHeaderRow}
                          >
                            <Plus className="h-3.5 w-3.5" strokeWidth={1.75} />
                            <span>添加请求头</span>
                          </Button>
                          <span className="text-[10px] text-[var(--text-muted)]">
                            {form.custom_headers.length}/{MAX_CUSTOM_HEADERS} 项
                          </span>
                        </div>
                      </>
                    ) : (
                      <div className="space-y-1.5">
                        <textarea
                          aria-label="自定义请求头批量文本"
                          rows={4}
                          value={rawHeadersText}
                          onChange={(e) => {
                            setConnectionTestResult(null);
                            setRawHeadersText(e.target.value);
                          }}
                          placeholder={
                            '{\n  "x-opencode-session": "{{session_id}}",\n  "User-Agent": "X-Custom-Client/1.0"\n}\n\n或逐行 Key: Value 格式（支持 # 注释）'
                          }
                          className="w-full rounded-[var(--radius-md)] border border-[var(--border-input)] bg-[var(--bg-panel)] p-2 font-mono text-[11px] leading-relaxed text-[var(--text-primary)] focus:border-[var(--border-focus)] focus:outline-none"
                        />
                        <div className="flex items-center justify-between text-[10px] text-[var(--text-muted)]">
                          <span>
                            支持标准 JSON 对象或逐行 Key: Value，保存或测试时自动校验
                          </span>
                          <span>
                            {activeHeaderCount}/{MAX_CUSTOM_HEADERS} 项
                          </span>
                        </div>
                      </div>
                    )}
                  </div>
                )}
              </div>

              <div className="col-span-2">
                <label className="flex cursor-pointer items-center gap-2 text-xs text-[var(--text-primary)]">
                  <Checkbox
                    checked={form.is_default}
                    onCheckedChange={(checked) =>
                      setForm({ ...form, is_default: checked === true })
                    }
                  />
                  <span>设为默认 VLM 配置</span>
                </label>
              </div>
            </div>

            {validationError && (
              <p className="mt-2 rounded-[var(--radius-sm)] bg-[var(--tint-failed)] px-2.5 py-1.5 text-[11px] text-[var(--status-failed)]">
                {validationError}
              </p>
            )}

            {connectionTestResult &&
              (connectionTestResult.ok ? (
                <div
                  role="status"
                  className="mt-2 flex items-center gap-1.5 rounded-[var(--radius-sm)] bg-[var(--tint-exported)] px-2.5 py-1.5 text-[11px] text-[var(--status-exported)]"
                >
                  <CheckCircle2 className="h-3.5 w-3.5 shrink-0" />
                  <span className="min-w-0 truncate">
                    连通成功（{connectionTestResult.data.latency_ms} ms）· 模型{' '}
                    {connectionTestResult.data.model} 已响应视觉请求
                  </span>
                </div>
              ) : (
                <div
                  role="alert"
                  className="mt-2 flex items-center gap-1.5 rounded-[var(--radius-sm)] bg-[var(--tint-failed)] px-2.5 py-1.5 text-[11px] text-[var(--status-failed)]"
                >
                  <XCircle className="h-3.5 w-3.5 shrink-0" />
                  <span className="min-w-0 break-all">
                    连通失败：{connectionTestResult.message}
                  </span>
                </div>
              ))}

            <div className="mt-auto pt-4">
              <p className="flex items-center gap-1.5 text-[11px] text-[var(--text-muted)]">
                <ShieldCheck className="h-3.5 w-3.5 shrink-0 text-[var(--status-exported)]" />
                <span>
                  密钥仅保存于系统 AppData 私有目录（providers.json，POSIX 0600 权限），不随视频或会话草稿同步。
                </span>
              </p>

              <DialogFooter className="justify-between">
                <Button
                  type="button"
                  variant="outline"
                  disabled={testingConnection || saving}
                  onClick={() => void handleTestConnection()}
                >
                  {testingConnection ? (
                    <Loader2 className="h-3.5 w-3.5 animate-spin" />
                  ) : (
                    <PlugZap className="h-3.5 w-3.5" strokeWidth={1.75} />
                  )}
                  <span>{testingConnection ? '测试中…' : '测试连通性'}</span>
                </Button>
                <div className="flex items-center gap-2">
                  <Button
                    type="button"
                    variant="ghost"
                    onClick={() => setVlmSettingsOpen(false)}
                  >
                    关闭
                  </Button>
                  <Button
                    type="submit"
                    variant="primary"
                    disabled={saving || testingConnection}
                  >
                    {saving ? '保存中…' : '保存配置'}
                  </Button>
                </div>
              </DialogFooter>
            </div>
          </form>
        </div>
      </DialogContent>
    </Dialog>
  );
}
