import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
  formatSrt,
  parseFlexibleTimecodeToMs,
  parseSrt,
  exportSrtFile,
  exportBatchZip,
} from './srt_formatter';
import { createZipArchive, computeCrc32 } from './zip';
import type { SubtitleEntry } from '../types/api';

describe('SRT Formatter & ZIP Archive Builder (Feature 12514)', () => {
  const sampleEntries: SubtitleEntry[] = [
    {
      index: 1,
      start_ms: 1000,
      end_ms: 2500,
      text: 'Hello world',
      confidence: 0.95,
    },
    {
      index: 2,
      start_ms: 3000,
      end_ms: 4800,
      text: 'SubLift Native Video Subtitle Extractor',
      confidence: 0.98,
    },
  ];

  it('formats subtitle entries into compliant SRT string', () => {
    const srt = formatSrt(sampleEntries);
    expect(srt).toContain('1\n00:00:01,000 --> 00:00:02,500\nHello world');
    expect(srt).toContain('2\n00:00:03,000 --> 00:00:04,800\nSubLift Native Video Subtitle Extractor');
  });

  it('returns empty string for empty entries array', () => {
    expect(formatSrt([])).toBe('');
  });

  it('parses flexible timecodes accurately across various representations', () => {
    expect(parseFlexibleTimecodeToMs('01:02:03,456')).toBe(3723456);
    expect(parseFlexibleTimecodeToMs('01:02:03.456')).toBe(3723456);
    expect(parseFlexibleTimecodeToMs('02:03.500')).toBe(123500);
    expect(parseFlexibleTimecodeToMs('10.5')).toBe(10500);
    expect(parseFlexibleTimecodeToMs('0')).toBe(0);
    expect(parseFlexibleTimecodeToMs('')).toBe(0);
  });

  it('parses formatted SRT text back into SubtitleEntry objects', () => {
    const srtText = `1
00:00:01,000 --> 00:00:02,500
Hello world

2
00:00:03,000 --> 00:00:04,800
SubLift OCR`;

    const parsed = parseSrt(srtText);
    expect(parsed.length).toBe(2);
    expect(parsed[0].start_ms).toBe(1000);
    expect(parsed[0].end_ms).toBe(2500);
    expect(parsed[0].text).toBe('Hello world');
    expect(parsed[1].start_ms).toBe(3000);
    expect(parsed[1].end_ms).toBe(4800);
    expect(parsed[1].text).toBe('SubLift OCR');
  });

  it('calculates standard CRC-32 checksum correctly', () => {
    // Known test vector: "123456789" -> 0xCBF43926
    const data = new TextEncoder().encode('123456789');
    const crc = computeCrc32(data);
    expect(crc).toBe(0xcbf43926);
  });

  it('creates valid PKZIP archive structure with correct headers and files', async () => {
    const files = [
      { name: 'video1.srt', content: '1\n00:00:00,000 --> 00:00:01,000\nSub 1\n' },
      { name: 'video2.srt', content: '1\n00:00:02,000 --> 00:00:03,000\nSub 2\n' },
    ];

    const blob = createZipArchive(files);
    expect(blob.type).toBe('application/zip');
    expect(blob.size).toBeGreaterThan(0);

    const arrayBuffer = await blob.arrayBuffer();
    const view = new DataView(arrayBuffer);
    const bytes = new Uint8Array(arrayBuffer);

    // Verify local file header signature PK\x03\x04 (0x04034b50)
    expect(view.getUint32(0, true)).toBe(0x04034b50);

    // Text decoding check for filename in raw byte stream
    const rawText = new TextDecoder('utf-8').decode(bytes);
    expect(rawText).toContain('video1.srt');
    expect(rawText).toContain('video2.srt');
    expect(rawText).toContain('Sub 1');
    expect(rawText).toContain('Sub 2');
  });

  describe('Browser Download Dispatches', () => {
    let appendedElement: any = null;
    let clicked = false;
    const originalDocument = globalThis.document;
    const originalUrl = globalThis.URL;

    beforeEach(() => {
      clicked = false;
      appendedElement = null;

      // Mock URL object methods
      const mockCreateObjectURL = vi.fn().mockReturnValue('blob:mock-url');
      const mockRevokeObjectURL = vi.fn();

      if (typeof globalThis.URL !== 'undefined') {
        globalThis.URL.createObjectURL = mockCreateObjectURL;
        globalThis.URL.revokeObjectURL = mockRevokeObjectURL;
      } else {
        (globalThis as any).URL = {
          createObjectURL: mockCreateObjectURL,
          revokeObjectURL: mockRevokeObjectURL,
        };
      }

      // Mock document and body
      const mockAnchor = {
        href: '',
        download: '',
        click: vi.fn(() => {
          clicked = true;
        }),
      };

      (globalThis as any).document = {
        createElement: vi.fn((tagName: string) => {
          if (tagName === 'a') return mockAnchor;
          return {};
        }),
        body: {
          appendChild: vi.fn((node: any) => {
            appendedElement = node;
            return node;
          }),
          removeChild: vi.fn((node: any) => node),
        },
      };
    });

    afterEach(() => {
      (globalThis as any).document = originalDocument;
      (globalThis as any).URL = originalUrl;
      vi.restoreAllMocks();
    });

    it('exports single SRT file via browser blob download', () => {
      exportSrtFile(sampleEntries, 'movie_scene.mp4');
      expect(globalThis.URL.createObjectURL).toHaveBeenCalled();
      expect(appendedElement).not.toBeNull();
      expect(appendedElement?.download).toBe('movie_scene.srt');
      expect(clicked).toBe(true);
      expect(globalThis.URL.revokeObjectURL).toHaveBeenCalledWith('blob:mock-url');
    });

    it('exports batch subtitles into a SINGLE zip download (no multiple sequential timeouts)', () => {
      const items = [
        { name: 'clip1.mp4', entries: sampleEntries },
        { name: 'clip2.mp4', entries: sampleEntries },
        { name: 'empty.mp4', entries: [] }, // empty should be skipped from zip
      ];

      const success = exportBatchZip(items, 'all_subtitles.zip');
      expect(success).toBe(true);
      expect(globalThis.URL.createObjectURL).toHaveBeenCalled();
      expect(appendedElement).not.toBeNull();
      expect(appendedElement?.download).toBe('all_subtitles.zip');
      expect(clicked).toBe(true);
    });

    it('returns false and skips download when all tasks have empty subtitles', () => {
      const items = [
        { name: 'empty1.mp4', entries: [] },
        { name: 'empty2.mp4', entries: [] },
      ];

      const success = exportBatchZip(items, 'empty_batch.zip');
      expect(success).toBe(false);
      expect(globalThis.URL.createObjectURL).not.toHaveBeenCalled();
      expect(clicked).toBe(false);
    });
  });
});
