import fs from 'fs';
import path from 'path';
import { describe, expect, it } from 'vitest';

import { BATCH_TASK_STATUSES } from './batch';

interface StatusesFixture {
  job: string[];
  task: string[];
  mapping: Record<string, string | null>;
}

const fixturePath = path.resolve(
  __dirname,
  '../../../../contracts/fixtures/statuses.json',
);
const fixture = JSON.parse(
  fs.readFileSync(fixturePath, 'utf-8'),
) as StatusesFixture;

describe('contracts/fixtures/statuses.json', () => {
  it('batch task statuses match the contract task list', () => {
    expect(new Set(BATCH_TASK_STATUSES)).toEqual(new Set(fixture.task));
  });

  it('mapping covers every task status', () => {
    for (const status of BATCH_TASK_STATUSES) {
      expect(fixture.mapping).toHaveProperty(status);
    }
  });
});
