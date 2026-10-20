import { describe, it, expect } from 'vitest';
import fs from 'fs';
import path from 'path';
import { formatSrt } from './srt_formatter';
import type { SubtitleEntry } from '../types/api';

interface SrtFixtureCase {
  name: string;
  entries: SubtitleEntry[];
  expected: string;
}

const fixturePath = path.resolve(__dirname, '../../../../contracts/fixtures/srt-cases.json');
const cases = JSON.parse(fs.readFileSync(fixturePath, 'utf-8')) as SrtFixtureCase[];

describe('SRT contract fixtures (Phase 17 D02)', () => {
  it('loads the shared fixture', () => {
    expect(cases.length).toBeGreaterThan(0);
  });

  for (const testCase of cases) {
    it(testCase.name, () => {
      expect(formatSrt(testCase.entries)).toBe(testCase.expected);
    });
  }
});
