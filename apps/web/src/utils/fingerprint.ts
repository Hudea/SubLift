/**
 * 本机指纹反查（Feature 12507）的客户端指纹。
 *
 * 浏览器拿不到用户本地文件的绝对路径，但文件与服务端通常在同一台机器上。
 * 这里提取 文件名 + 字节数 + 首尾各 4KB 的原始字节（hex），交给
 * POST /api/video/resolve 在服务端受控目录内做逐字节比对定位原文件。
 * 用原始字节而非哈希：crypto.subtle 对大文件也要全量读，而这里只读边缘，
 * 且字节比对的强度与哈希等价（内容本就由持有者提供）。
 */

export const EDGE_CHUNK_BYTES = 4096;
/** 与服务端约定：size > 2 * EDGE_CHUNK_BYTES 时必须提供尾部字节 */
const TAIL_THRESHOLD = 2 * EDGE_CHUNK_BYTES;

export interface FileFingerprint {
  name: string;
  size: number;
  head_hex: string;
  tail_hex: string;
}

function toHex(buf: ArrayBuffer): string {
  const bytes = new Uint8Array(buf);
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += bytes[i].toString(16).padStart(2, '0');
  }
  return out;
}

export async function computeFingerprint(file: File): Promise<FileFingerprint> {
  const headBuf =
    file.size <= EDGE_CHUNK_BYTES
      ? await file.arrayBuffer()
      : await file.slice(0, EDGE_CHUNK_BYTES).arrayBuffer();
  const tailBuf =
    file.size > TAIL_THRESHOLD ? await file.slice(file.size - EDGE_CHUNK_BYTES).arrayBuffer() : null;

  return {
    name: file.name,
    size: file.size,
    head_hex: toHex(headBuf),
    tail_hex: tailBuf ? toHex(tailBuf) : '',
  };
}
