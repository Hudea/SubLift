/**
 * SubLift Pure TypeScript / JavaScript Zero-Dependency ZIP Archive Builder
 * Implements standard PKZIP 2.0 (STORE method, UTF-8 filenames, CRC32 checksums)
 */

export interface ZipArchiveFile {
  name: string;
  content: string | Uint8Array;
}

// CRC-32 Lookup Table
const crc32Table = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let c = i;
  for (let k = 0; k < 8; k++) {
    c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  }
  crc32Table[i] = c;
}

export function computeCrc32(data: Uint8Array): number {
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) {
    crc = (crc >>> 8) ^ crc32Table[(crc ^ data[i]) & 0xff];
  }
  return (crc ^ 0xffffffff) >>> 0;
}

/**
 * Builds a valid ZIP archive Blob from an array of files.
 */
export function createZipArchive(files: ZipArchiveFile[]): Blob {
  const textEncoder = new TextEncoder();
  const fileEntries = files.map((file) => {
    const nameBytes = textEncoder.encode(file.name);
    const dataBytes =
      typeof file.content === 'string' ? textEncoder.encode(file.content) : file.content;
    const crc = computeCrc32(dataBytes);
    return {
      name: file.name,
      nameBytes,
      dataBytes,
      crc,
      size: dataBytes.length,
    };
  });

  // Calculate total buffer size needed
  let totalLocalHeadersSize = 0;
  for (const entry of fileEntries) {
    totalLocalHeadersSize += 30 + entry.nameBytes.length + entry.size;
  }

  let totalCentralDirSize = 0;
  for (const entry of fileEntries) {
    totalCentralDirSize += 46 + entry.nameBytes.length;
  }

  const eocdSize = 22;
  const totalZipSize = totalLocalHeadersSize + totalCentralDirSize + eocdSize;

  const buffer = new ArrayBuffer(totalZipSize);
  const view = new DataView(buffer);
  const uint8 = new Uint8Array(buffer);

  let offset = 0;
  const localOffsets: number[] = [];

  // 1. Write Local File Headers and Data
  for (const entry of fileEntries) {
    localOffsets.push(offset);

    // Signature: PK\x03\x04
    view.setUint32(offset, 0x04034b50, true);
    // Version needed: 20 (2.0)
    view.setUint16(offset + 4, 20, true);
    // Flags: 0x0800 (UTF-8 filename)
    view.setUint16(offset + 6, 0x0800, true);
    // Compression: 0 (Store / no compression)
    view.setUint16(offset + 8, 0, true);
    // Mod time / date: 0
    view.setUint16(offset + 10, 0, true);
    view.setUint16(offset + 12, 0, true);
    // CRC-32
    view.setUint32(offset + 14, entry.crc, true);
    // Compressed size
    view.setUint32(offset + 18, entry.size, true);
    // Uncompressed size
    view.setUint32(offset + 22, entry.size, true);
    // Filename length
    view.setUint16(offset + 26, entry.nameBytes.length, true);
    // Extra field length
    view.setUint16(offset + 28, 0, true);

    offset += 30;

    // Filename
    uint8.set(entry.nameBytes, offset);
    offset += entry.nameBytes.length;

    // File Data
    uint8.set(entry.dataBytes, offset);
    offset += entry.size;
  }

  const centralDirStartOffset = offset;

  // 2. Write Central Directory Headers
  for (let i = 0; i < fileEntries.length; i++) {
    const entry = fileEntries[i];
    const localOffset = localOffsets[i];

    // Signature: PK\x01\x02
    view.setUint32(offset, 0x02014b50, true);
    // Version made by: 20 (UNIX / DOS compatible)
    view.setUint16(offset + 4, 20, true);
    // Version needed: 20
    view.setUint16(offset + 6, 20, true);
    // Flags: 0x0800 (UTF-8)
    view.setUint16(offset + 8, 0x0800, true);
    // Compression: 0 (Store)
    view.setUint16(offset + 10, 0, true);
    // Mod time / date
    view.setUint16(offset + 12, 0, true);
    view.setUint16(offset + 14, 0, true);
    // CRC-32
    view.setUint32(offset + 16, entry.crc, true);
    // Compressed size
    view.setUint32(offset + 20, entry.size, true);
    // Uncompressed size
    view.setUint32(offset + 24, entry.size, true);
    // Filename length
    view.setUint16(offset + 28, entry.nameBytes.length, true);
    // Extra field length
    view.setUint16(offset + 30, 0, true);
    // File comment length
    view.setUint16(offset + 32, 0, true);
    // Disk number start
    view.setUint16(offset + 34, 0, true);
    // Internal file attributes
    view.setUint16(offset + 36, 0, true);
    // External file attributes
    view.setUint32(offset + 38, 0, true);
    // Relative offset of local header
    view.setUint32(offset + 42, localOffset, true);

    offset += 46;

    // Filename
    uint8.set(entry.nameBytes, offset);
    offset += entry.nameBytes.length;
  }

  const centralDirEndOffset = offset;
  const centralDirSize = centralDirEndOffset - centralDirStartOffset;

  // 3. Write End of Central Directory (EOCD)
  // Signature: PK\x05\x06
  view.setUint32(offset, 0x06054b50, true);
  // Number of this disk: 0
  view.setUint16(offset + 4, 0, true);
  // Disk where CD starts: 0
  view.setUint16(offset + 6, 0, true);
  // Number of CD records on this disk
  view.setUint16(offset + 8, fileEntries.length, true);
  // Total number of CD records
  view.setUint16(offset + 10, fileEntries.length, true);
  // Size of CD
  view.setUint32(offset + 12, centralDirSize, true);
  // Offset of CD
  view.setUint32(offset + 16, centralDirStartOffset, true);
  // Comment length: 0
  view.setUint16(offset + 20, 0, true);

  return new Blob([buffer], { type: 'application/zip' });
}

/**
 * Triggers browser download of a single ZIP archive.
 */
export function downloadZipBlob(blob: Blob, filename: string): void {
  const cleanName = filename.endsWith('.zip') ? filename : `${filename}.zip`;
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = cleanName;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}
