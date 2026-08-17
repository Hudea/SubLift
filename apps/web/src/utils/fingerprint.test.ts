import { describe, it, expect } from 'vitest';
import { computeFingerprint, EDGE_CHUNK_BYTES } from './fingerprint';

function toHex(bytes: Uint8Array): string {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}

/** Node 20 的 File 构造器可用于模拟浏览器 File（Blob 语义一致） */
function makeFile(bytes: Uint8Array, name: string): File {
  return new File([bytes as BlobPart], name);
}

describe('computeFingerprint (Feature 12507 指纹反查)', () => {
  it('小文件（<= 4KB）：head 覆盖全文件，tail 为空', async () => {
    const data = new Uint8Array([0x00, 0x01, 0x02, 0xff, 0x41]);
    const fp = await computeFingerprint(makeFile(data, 'small.mp4'));

    expect(fp.name).toBe('small.mp4');
    expect(fp.size).toBe(5);
    expect(fp.head_hex).toBe(toHex(data));
    expect(fp.tail_hex).toBe('');
  });

  it('大文件（> 8KB）：head/tail 各取首尾 4KB 且逐字节正确', async () => {
    // 12300 - 4096 = 8204，不是 256 的倍数，保证首尾相位不同（可区分）
    const size = 12300;
    const data = new Uint8Array(size);
    for (let i = 0; i < size; i++) data[i] = i % 251;

    const fp = await computeFingerprint(makeFile(data, 'big.mp4'));

    expect(fp.size).toBe(size);
    expect(fp.head_hex).toBe(toHex(data.subarray(0, EDGE_CHUNK_BYTES)));
    expect(fp.tail_hex).toBe(toHex(data.subarray(size - EDGE_CHUNK_BYTES)));
    // 首尾内容不同，指纹必须可区分
    expect(fp.head_hex).not.toBe(fp.tail_hex);
  });

  it('中等文件（4KB < size <= 8KB）：head 取满 4KB，无 tail', async () => {
    const size = 6000;
    const data = new Uint8Array(size).fill(0xab);
    const fp = await computeFingerprint(makeFile(data, 'mid.mp4'));

    expect(fp.head_hex).toBe(toHex(data.subarray(0, EDGE_CHUNK_BYTES)));
    expect(fp.tail_hex).toBe('');
  });
});
