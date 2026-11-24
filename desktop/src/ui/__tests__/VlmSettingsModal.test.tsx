import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from 'vitest';
import type { ProviderProfileMaskedDto } from '../../core/types';
import { useWorkbenchStore } from '../../store';
import App from '../App';
import {
  formatCustomHeadersAsText,
  parseBulkCustomHeadersText,
  parsePastedCustomHeaders,
} from '../workbench/VlmSettingsModal';
import { installJsdomPolyfills, resetWorkbenchStore } from './testHarness';

beforeAll(() => {
  installJsdomPolyfills();
});

describe('VlmSettingsModal', () => {
  beforeEach(() => {
    Object.defineProperty(window, 'innerWidth', {
      configurable: true,
      writable: true,
      value: 1440,
    });
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('preserves masked key via api_key_update: keep, supports replace/clear, preset auto-fill, .env import, and focus restoration', async () => {
    const bridge = resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    const gearBtn = screen.getByRole('button', { name: '打开 VLM 设置' });
    gearBtn.focus();
    fireEvent.click(gearBtn);

    expect(screen.getByText('VLM 多配置管理')).toBeInTheDocument();
    expect(screen.getByText('当前已保存密钥: sk-****9f2a')).toBeInTheDocument();

    // Save without typing new API key -> api_key_update.mode === 'keep'
    const modelInput = screen.getByLabelText('模型标识 (Model)') as HTMLInputElement;
    fireEvent.change(modelInput, { target: { value: 'MiniMax-M3.1-Pro' } });
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));

    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          id: 'prov_1',
          model: 'MiniMax-M3.1-Pro',
          api_key_update: { mode: 'keep' },
        })
      );
    });

    // Replace API key
    const keyInput = screen.getByLabelText('API Key') as HTMLInputElement;
    fireEvent.change(keyInput, { target: { value: 'sk-new-secret-1234' } });
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));

    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          api_key_update: { mode: 'replace', value: 'sk-new-secret-1234' },
        })
      );
    });

    // Clear API key
    fireEvent.click(screen.getByRole('button', { name: '清除密钥' }));
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));
    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          api_key_update: { mode: 'clear' },
        })
      );
    });

    // Import from .env
    fireEvent.click(screen.getByRole('button', { name: '从 .env 导入' }));
    await waitFor(() => {
      expect(bridge.importProvidersFromEnv).toHaveBeenCalled();
    });

    // Esc closes modal and Radix restores focus to opener
    fireEvent.keyDown(screen.getByRole('dialog'), { key: 'Escape' });
    await waitFor(() => {
      expect(screen.queryByRole('dialog')).toBeNull();
    });
  });

  it('uses OpenAI compatible copy & default blank preset and syncs top-bar pill when switching default provider', async () => {
    let statefulProviders: ProviderProfileMaskedDto[] = [
      {
        id: 'env-minimax',
        name: 'MiniMax (env)',
        provider_type: 'minimax',
        base_url: 'https://api.minimax.cn/v1',
        model: 'MiniMax-M3.1-Flash',
        thinking_mode: 'auto',
        concurrency: 6,
        has_api_key: true,
        masked_api_key: 'sk-****1111',
        is_default: true,
      },
      {
        id: 'env-nvidia',
        name: 'NVIDIA NIM (env)',
        provider_type: 'nvidia',
        base_url: 'https://integrate.api.nvidia.com/v1',
        model: 'nvidia/nemotron-3.5-super-vl-120b-a12b',
        thinking_mode: 'auto',
        concurrency: 4,
        has_api_key: true,
        masked_api_key: 'nva****q1xc',
        is_default: false,
      },
    ];

    const bridge = resetWorkbenchStore({
      listProviders: vi.fn(async () => statefulProviders.map((p) => ({ ...p }))),
      saveProvider: vi.fn(async (profile) => {
        const targetId = profile.id ?? 'prov_new';
        if (profile.is_default) {
          statefulProviders = statefulProviders.map((p) => ({
            ...p,
            is_default: p.id === targetId,
          }));
        }
        return { ...statefulProviders.find((p) => p.id === targetId)! };
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    await act(async () => {
      await useWorkbenchStore.getState().addVideoPaths(['/v/ep01.mkv']);
    });

    // Switch engine to VLM -> top bar shows MiniMax (env)
    fireEvent.click(screen.getByRole('button', { name: 'VLM' }));
    expect(screen.getByRole('button', { name: 'MiniMax (env)' })).toBeInTheDocument();

    // Open VLM settings modal
    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    expect(screen.getByText(/支持兼容 OpenAI/)).toBeInTheDocument();
    expect(screen.getByText(/的视觉模型端点/)).toBeInTheDocument();
    expect(screen.queryByText(/MiniMax、NVIDIA NIM 及任意/)).toBeNull();

    // Clicking 新建配置 defaults to OpenAI 兼容 with matching OpenAI placeholders
    fireEvent.click(screen.getByRole('button', { name: '新建配置' }));
    expect(screen.getByLabelText('供应商预设')).toHaveTextContent('OpenAI 兼容');
    expect(screen.getByLabelText('配置名称')).toHaveAttribute('placeholder', '例如：OpenAI 兼容服务');
    expect(screen.getByLabelText('API Base URL')).toHaveAttribute('placeholder', 'https://api.openai.com/v1');
    expect(screen.getByLabelText('模型标识 (Model)')).toHaveAttribute('placeholder', 'gpt-4o-mini');
    expect(screen.getByLabelText('API Base URL')).toHaveValue('');
    expect(screen.getByLabelText('模型标识 (Model)')).toHaveValue('');

    // Select NVIDIA NIM (env), set as default, and save
    fireEvent.click(screen.getByText('NVIDIA NIM (env)'));
    fireEvent.click(screen.getByRole('checkbox', { name: '设为默认 VLM 配置' }));
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));

    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          id: 'env-nvidia',
          is_default: true,
        })
      );
      expect(useWorkbenchStore.getState().tasks[0].providerId).toBe('env-nvidia');
    });

    // Close modal -> top-bar pill reflects NVIDIA NIM (env) instead of MiniMax (env)
    fireEvent.click(screen.getByRole('button', { name: '关闭' }));
    expect(screen.getByRole('button', { name: 'NVIDIA NIM (env)' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'MiniMax (env)' })).toBeNull();

    // Reopening modal automatically selects active default NVIDIA NIM (env)
    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    expect(screen.getByLabelText('配置名称')).toHaveValue('NVIDIA NIM (env)');
  });

  it('auto-dismisses toast notifications after default timeout and pauses auto-dismiss while hovered', async () => {
    resetWorkbenchStore();
    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    vi.useFakeTimers();

    act(() => {
      useWorkbenchStore.getState().pushToast({
        severity: 'success',
        title: '已保存 VLM 配置「NVIDIA NIM」',
      });
    });

    const toastTitle = screen.getByText('已保存 VLM 配置「NVIDIA NIM」');
    const toastCard = toastTitle.closest('[role="status"]')!;
    expect(toastCard).toBeInTheDocument();

    // Hovering pauses auto-dismiss even past default 3500ms
    fireEvent.mouseEnter(toastCard);
    await act(async () => {
      await vi.advanceTimersByTimeAsync(4000);
    });
    expect(screen.getByText('已保存 VLM 配置「NVIDIA NIM」')).toBeInTheDocument();

    // Leaving mouse restarts timer and auto-dismisses after 3500ms
    fireEvent.mouseLeave(toastCard);
    await act(async () => {
      await vi.advanceTimersByTimeAsync(3600);
    });
    expect(screen.queryByText('已保存 VLM 配置「NVIDIA NIM」')).toBeNull();
  });

  it('parses clipboard custom headers from Key: Value and JSON while ignoring plain URLs', () => {
    expect(
      parsePastedCustomHeaders(
        '# comment\n\nx-opencode-session: {{session_id}}\nUser-Agent: X-Custom-Client/1.0\nHTTP-Referer: https://sublift.app'
      )
    ).toEqual([
      { name: 'x-opencode-session', value: '{{session_id}}' },
      { name: 'User-Agent', value: 'X-Custom-Client/1.0' },
      { name: 'HTTP-Referer', value: 'https://sublift.app' },
    ]);

    expect(
      parsePastedCustomHeaders(
        '{"x-opencode-session": "{{session_id}}", "X-Title": "SubLift"}'
      )
    ).toEqual([
      { name: 'x-opencode-session', value: '{{session_id}}' },
      { name: 'X-Title', value: 'SubLift' },
    ]);

    expect(parsePastedCustomHeaders('https://api.example.com/v1')).toBeNull();
    expect(parsePastedCustomHeaders('http://127.0.0.1:8080/v1')).toBeNull();
    expect(parsePastedCustomHeaders('not a header')).toBeNull();
    expect(parsePastedCustomHeaders('Bad Header: value')).toBeNull();
  });

  it('supports collapsible custom headers with multiline paste, reserved header rejection, save payload, and reopen auto-expand', async () => {
    let statefulProviders: ProviderProfileMaskedDto[] = [
      {
        id: 'prov_gateway',
        name: 'Custom Gateway',
        provider_type: 'openai',
        base_url: 'https://gateway.example.com/v1',
        model: 'gpt-4o-mini',
        thinking_mode: 'auto',
        concurrency: 4,
        has_api_key: true,
        masked_api_key: 'sk-****1234',
        is_default: true,
        custom_headers: [],
      },
    ];

    const bridge = resetWorkbenchStore({
      listProviders: vi.fn(async () => statefulProviders.map((p) => ({ ...p }))),
      saveProvider: vi.fn(async (profile) => {
        const targetId = profile.id ?? 'prov_gateway';
        statefulProviders = statefulProviders.map((p) =>
          p.id === targetId
            ? {
                ...p,
                name: profile.name,
                provider_type: profile.provider_type,
                base_url: profile.base_url,
                model: profile.model,
                thinking_mode: profile.thinking_mode,
                concurrency: profile.concurrency,
                is_default: profile.is_default,
                custom_headers: profile.custom_headers ?? [],
              }
            : p
        );
        return { ...statefulProviders.find((p) => p.id === targetId)! };
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    // Open VLM settings modal: custom headers section starts collapsed
    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    const toggleBtn = screen.getByRole('button', { name: /自定义请求头（可选）/ });
    expect(toggleBtn).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByLabelText('请求头名称 1')).toBeNull();

    // Expand section -> seeds 1 empty row & shows {{session_id}} hint
    fireEvent.click(toggleBtn);
    expect(toggleBtn).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByText('{{session_id}}')).toBeInTheDocument();

    // Multi-line Key: Value paste into Name splits into 2 rows
    const nameInput1 = screen.getByLabelText('请求头名称 1');
    fireEvent.paste(nameInput1, {
      clipboardData: {
        getData: () =>
          'x-opencode-session: {{session_id}}\nUser-Agent: X-Custom-Client/1.0',
      },
    });
    expect(screen.getByLabelText('请求头名称 1')).toHaveValue(
      'x-opencode-session'
    );
    expect(screen.getByLabelText('请求头值 1')).toHaveValue('{{session_id}}');
    expect(screen.getByLabelText('请求头名称 2')).toHaveValue('User-Agent');
    expect(screen.getByLabelText('请求头值 2')).toHaveValue(
      'X-Custom-Client/1.0'
    );
    expect(screen.getByText('已配置 2 项')).toBeInTheDocument();

    // Adding reserved transport header Host is blocked before RPC save
    fireEvent.click(screen.getByRole('button', { name: '添加请求头' }));
    fireEvent.change(screen.getByLabelText('请求头名称 3'), {
      target: { value: 'Host' },
    });
    fireEvent.change(screen.getByLabelText('请求头值 3'), {
      target: { value: 'evil.example.com' },
    });
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));
    expect(screen.getByText(/属于底层传输保留头/)).toBeInTheDocument();
    expect(bridge.saveProvider).not.toHaveBeenCalled();

    // Remove invalid row and save -> payload carries custom_headers
    fireEvent.click(screen.getByRole('button', { name: '删除请求头 3' }));
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));
    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          id: 'prov_gateway',
          custom_headers: [
            { name: 'x-opencode-session', value: '{{session_id}}' },
            { name: 'User-Agent', value: 'X-Custom-Client/1.0' },
          ],
        })
      );
    });

    // Close and reopen -> auto-expands because custom headers are configured
    fireEvent.click(screen.getByRole('button', { name: '关闭' }));
    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    expect(
      screen.getByRole('button', { name: /自定义请求头（可选）/ })
    ).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByText('已配置 2 项')).toBeInTheDocument();
    expect(screen.getByLabelText('请求头名称 1')).toHaveValue(
      'x-opencode-session'
    );
  });

  it('formats and validates bulk custom headers between JSON/text and KV representation', () => {
    expect(formatCustomHeadersAsText([])).toBe('');
    expect(
      formatCustomHeadersAsText([
        { name: 'x-opencode-session', value: '{{session_id}}' },
        { name: 'User-Agent', value: 'X-Custom-Client/1.0' },
      ])
    ).toBe(
      '{\n  "x-opencode-session": "{{session_id}}",\n  "User-Agent": "X-Custom-Client/1.0"\n}'
    );
    expect(
      formatCustomHeadersAsText([
        { name: 'X-Dup', value: '1' },
        { name: 'x-dup', value: '2' },
      ])
    ).toBe('X-Dup: 1\nx-dup: 2');

    expect(parseBulkCustomHeadersText('   ')).toEqual({ ok: true, headers: [] });
    expect(
      parseBulkCustomHeadersText('{"x-opencode-session": "{{session_id}}"}')
    ).toEqual({
      ok: true,
      headers: [{ name: 'x-opencode-session', value: '{{session_id}}' }],
    });
    expect(
      parseBulkCustomHeadersText('# note\nx-title: SubLift\nUser-Agent: Test/1.0')
    ).toEqual({
      ok: true,
      headers: [
        { name: 'x-title', value: 'SubLift' },
        { name: 'User-Agent', value: 'Test/1.0' },
      ],
    });
    expect(parseBulkCustomHeadersText('{bad json').ok).toBe(false);
    expect(parseBulkCustomHeadersText('https://api.example.com/v1').ok).toBe(false);
    expect(parseBulkCustomHeadersText('missing colon line').ok).toBe(false);
    expect(parseBulkCustomHeadersText('Host: evil.example.com').ok).toBe(false);
  });

  it('supports JSON/text custom header mode switching, invalid JSON guard, and direct save', async () => {
    let statefulProviders: ProviderProfileMaskedDto[] = [
      {
        id: 'prov_dual',
        name: 'Dual Gateway',
        provider_type: 'openai',
        base_url: 'https://gateway.example.com/v1',
        model: 'gpt-4o-mini',
        thinking_mode: 'auto',
        concurrency: 4,
        has_api_key: true,
        masked_api_key: 'sk-****1234',
        is_default: true,
        custom_headers: [],
      },
    ];

    const bridge = resetWorkbenchStore({
      listProviders: vi.fn(async () => statefulProviders.map((p) => ({ ...p }))),
      saveProvider: vi.fn(async (profile) => {
        statefulProviders = statefulProviders.map((p) =>
          p.id === (profile.id ?? 'prov_dual')
            ? { ...p, custom_headers: profile.custom_headers ?? [] }
            : p
        );
        return { ...statefulProviders[0] };
      }),
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    fireEvent.click(screen.getByRole('button', { name: /自定义请求头（可选）/ }));

    // Switch KV -> JSON / 文本
    fireEvent.click(screen.getByRole('button', { name: 'JSON / 文本' }));
    const textarea = screen.getByLabelText('自定义请求头批量文本');
    expect(textarea).toBeInTheDocument();

    // Invalid JSON blocks switching back to KV and shows error
    fireEvent.change(textarea, { target: { value: '{"broken": ' } });
    fireEvent.click(screen.getByRole('button', { name: '键值表单' }));
    expect(screen.getByText(/JSON 格式无效/)).toBeInTheDocument();
    expect(screen.getByLabelText('自定义请求头批量文本')).toBeInTheDocument();

    // Valid JSON switches cleanly to KV rows
    fireEvent.change(textarea, {
      target: {
        value:
          '{"x-opencode-session": "{{session_id}}", "User-Agent": "X-Custom-Client/1.0"}',
      },
    });
    fireEvent.click(screen.getByRole('button', { name: '键值表单' }));
    expect(screen.getByLabelText('请求头名称 1')).toHaveValue(
      'x-opencode-session'
    );
    expect(screen.getByLabelText('请求头值 1')).toHaveValue('{{session_id}}');
    expect(screen.getByText('已配置 2 项')).toBeInTheDocument();

    // Switch back to JSON / 文本 and save directly without switching back to KV
    fireEvent.click(screen.getByRole('button', { name: 'JSON / 文本' }));
    fireEvent.change(screen.getByLabelText('自定义请求头批量文本'), {
      target: { value: 'x-opencode-session: {{session_id}}\nX-Title: DirectSave' },
    });
    fireEvent.click(screen.getByRole('button', { name: '保存配置' }));

    await waitFor(() => {
      expect(bridge.saveProvider).toHaveBeenCalledWith(
        expect.objectContaining({
          id: 'prov_dual',
          custom_headers: [
            { name: 'x-opencode-session', value: '{{session_id}}' },
            { name: 'X-Title', value: 'DirectSave' },
          ],
        })
      );
    });
  });

  it('tests provider connectivity without saving, shows latency or error banner, and clears stale status on edit', async () => {
    const testConnectionMock = vi
      .fn()
      .mockResolvedValueOnce({
        latency_ms: 88,
        endpoint: 'https://gateway.example.com/v1/chat/completions',
        model: 'gpt-4o-mini',
        response_preview: 'OK',
      })
      .mockRejectedValueOnce(
        new Error('[VlmAuthError] HTTP 401: invalid api key')
      );

    const bridge = resetWorkbenchStore({
      testProviderConnection: testConnectionMock,
    });

    render(<App />);
    await waitFor(() => {
      expect(useWorkbenchStore.getState().initialized).toBe(true);
    });

    fireEvent.click(screen.getByRole('button', { name: '打开 VLM 设置' }));
    fireEvent.click(screen.getByRole('button', { name: '新建配置' }));

    // Leave name empty intentionally to verify non-mutating connectivity probe does not require name
    fireEvent.change(screen.getByLabelText('API Base URL'), {
      target: { value: 'https://gateway.example.com/v1' },
    });
    fireEvent.change(screen.getByLabelText('模型标识 (Model)'), {
      target: { value: 'gpt-4o-mini' },
    });
    fireEvent.change(screen.getByLabelText('API Key'), {
      target: { value: 'sk-probe-123' },
    });

    fireEvent.click(screen.getByRole('button', { name: /自定义请求头（可选）/ }));
    fireEvent.click(screen.getByRole('button', { name: 'JSON / 文本' }));
    fireEvent.change(screen.getByLabelText('自定义请求头批量文本'), {
      target: { value: '{"x-opencode-session": "{{session_id}}"}' },
    });

    fireEvent.click(screen.getByRole('button', { name: '测试连通性' }));

    await waitFor(() => {
      expect(testConnectionMock).toHaveBeenCalledTimes(1);
      expect(screen.getByRole('status')).toHaveTextContent(
        '连通成功（88 ms）· 模型 gpt-4o-mini 已响应视觉请求'
      );
    });
    expect(testConnectionMock).toHaveBeenCalledWith(
      expect.objectContaining({
        name: '',
        base_url: 'https://gateway.example.com/v1',
        model: 'gpt-4o-mini',
        api_key_update: { mode: 'replace', value: 'sk-probe-123' },
        custom_headers: [{ name: 'x-opencode-session', value: '{{session_id}}' }],
      })
    );
    expect(bridge.saveProvider).not.toHaveBeenCalled();

    // Editing connection inputs clears previous connectivity status
    fireEvent.change(screen.getByLabelText('API Base URL'), {
      target: { value: 'https://gateway-2.example.com/v1' },
    });
    expect(screen.queryByRole('status')).toBeNull();

    // Failing probe shows inline alert banner
    fireEvent.click(screen.getByRole('button', { name: '测试连通性' }));
    await waitFor(() => {
      expect(screen.getByRole('alert')).toHaveTextContent(
        '连通失败：HTTP 401: invalid api key'
      );
    });
  });
});
