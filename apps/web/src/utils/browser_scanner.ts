import { computeFingerprint } from './fingerprint';
import { SubLiftApiClient } from '../api/client';
import type {
  BatchAcceptedItem,
  BatchScanRejection,
} from '../types/batch';

export interface ScanFileEntry {
  file: File;
  relativeDir?: string;
}

/**
 * 08103 / B02 对齐：浏览器侧多模态目录递归展开与输入扫描器
 */
export class BrowserDirectoryScanner {
  private static readonly SUPPORTED_EXTENSIONS = new Set([
    'mp4',
    'mov',
    'mkv',
    'webm',
    'avi',
    'm4v',
    'flv',
  ]);

  private static readonly MAX_SCAN_DEPTH = 15;
  private static readonly MAX_TOTAL_FILES = 10000;

  /**
   * 判断扩展名是否为受支持的媒体格式
   */
  static isSupportedMedia(nameOrExt: string): boolean {
    const ext = (nameOrExt.split('.').pop() || '').toLowerCase();
    return this.SUPPORTED_EXTENSIONS.has(ext);
  }

  /**
   * 递归读取单个目录的所有条目（循环读取直至返回空数组，彻底防御超过 100 个文件时的截断丢件）
   */
  static async readAllDirectoryEntries(
    reader: FileSystemDirectoryReader
  ): Promise<FileSystemEntry[]> {
    const entries: FileSystemEntry[] = [];
    while (true) {
      const batch = await new Promise<FileSystemEntry[]>((resolve, reject) => {
        reader.readEntries(resolve, reject);
      });
      if (!batch || batch.length === 0) break;
      entries.push(...batch);
    }
    return entries;
  }

  /**
   * 递归扫描 DataTransferItemList 或 FileList
   */
  static async scanDataTransferItems(
    items: DataTransferItemList | FileList
  ): Promise<{ files: ScanFileEntry[]; skipped: number; rejected: BatchScanRejection[] }> {
    const validFiles: ScanFileEntry[] = [];
    let skipped = 0;
    const rejected: BatchScanRejection[] = [];
    const visitedPaths = new Set<string>();

    if ('length' in items && items.length > 0 && 'webkitGetAsEntry' in (items[0] as any)) {
      const transferItems = items as DataTransferItemList;
      const rootEntries: FileSystemEntry[] = [];
      for (let i = 0; i < transferItems.length; i++) {
        const entry = transferItems[i].webkitGetAsEntry();
        if (entry) rootEntries.push(entry);
      }

      for (const rootEntry of rootEntries) {
        await this.traverseEntry(
          rootEntry,
          '',
          0,
          validFiles,
          (cnt) => (skipped += cnt),
          rejected,
          visitedPaths
        );
      }
    } else {
      // 普通 FileList (无 webkitGetAsEntry 时 fallback)
      const fileList = items as FileList;
      for (let i = 0; i < fileList.length; i++) {
        const file = fileList[i];
        if (file.name.startsWith('.')) {
          skipped++;
          continue;
        }
        const ext = (file.name.split('.').pop() || '').toLowerCase();
        if (this.SUPPORTED_EXTENSIONS.has(ext)) {
          validFiles.push({ file });
        } else {
          rejected.push({
            pathOrName: file.name,
            reason: { kind: 'unsupportedFormat', extension: ext },
          });
        }
      }
    }

    return { files: validFiles, skipped, rejected };
  }

  private static async traverseEntry(
    entry: FileSystemEntry,
    currentPath: string,
    depth: number,
    outFiles: ScanFileEntry[],
    onSkipped: (count: number) => void,
    outRejected: BatchScanRejection[],
    visitedPaths: Set<string>
  ): Promise<void> {
    if (depth > this.MAX_SCAN_DEPTH || outFiles.length >= this.MAX_TOTAL_FILES) {
      onSkipped(1);
      return;
    }

    // 排除隐藏文件与隐藏目录 (如 .DS_Store, .git, ._*)
    if (entry.name.startsWith('.')) {
      onSkipped(1);
      return;
    }

    const fullVirtualPath = `${currentPath}/${entry.name}`;
    if (visitedPaths.has(fullVirtualPath)) {
      onSkipped(1);
      return;
    }
    visitedPaths.add(fullVirtualPath);

    if (entry.isFile) {
      const file = await new Promise<File>((resolve, reject) => {
        (entry as FileSystemFileEntry).file(resolve, reject);
      });
      const ext = (file.name.split('.').pop() || '').toLowerCase();
      if (this.SUPPORTED_EXTENSIONS.has(ext)) {
        outFiles.push({
          file,
          relativeDir: currentPath ? `${currentPath.replace(/^\//, '')}/` : undefined,
        });
      } else {
        outRejected.push({
          pathOrName: file.name,
          reason: { kind: 'unsupportedFormat', extension: ext },
        });
      }
    } else if (entry.isDirectory) {
      // macOS 专属包结构 (如 .app, .photoslibrary) 视为 package 静默跳过
      if (entry.name.endsWith('.app') || entry.name.endsWith('.photoslibrary')) {
        onSkipped(1);
        return;
      }

      const dirReader = (entry as FileSystemDirectoryEntry).createReader();
      try {
        const children = await this.readAllDirectoryEntries(dirReader);
        if (children.length === 0) {
          outRejected.push({
            pathOrName: entry.name,
            reason: { kind: 'emptyDirectory' },
          });
          return;
        }
        for (const child of children) {
          await this.traverseEntry(
            child,
            fullVirtualPath,
            depth + 1,
            outFiles,
            onSkipped,
            outRejected,
            visitedPaths
          );
        }
      } catch (err: any) {
        outRejected.push({
          pathOrName: entry.name,
          reason: { kind: 'unreadable', detail: err?.message || String(err) },
        });
      }
    }
  }

  /**
   * 带并发控制 (Concurrency: 4) 的指纹计算与服务端路径反查
   */
  static async resolveFilesWithConcurrency(
    scanEntries: ScanFileEntry[],
    existingPaths: Set<string>,
    concurrency = 4
  ): Promise<{ accepted: BatchAcceptedItem[]; rejected: BatchScanRejection[] }> {
    const accepted: BatchAcceptedItem[] = [];
    const rejected: BatchScanRejection[] = [];
    const seenPaths = new Set<string>(existingPaths);

    let idx = 0;
    async function worker() {
      while (idx < scanEntries.length) {
        const item = scanEntries[idx++];
        const file = item.file;
        const nativePath = (file as File & { path?: string }).path;

        let resolvedPath: string | null = nativePath || null;
        if (!resolvedPath) {
          try {
            const fp = await computeFingerprint(file);
            resolvedPath = await SubLiftApiClient.resolveVideoPath(fp);
          } catch {
            resolvedPath = null;
          }
        }

        if (resolvedPath) {
          if (seenPaths.has(resolvedPath)) {
            rejected.push({
              pathOrName: file.name,
              reason: { kind: 'duplicate' },
            });
          } else {
            seenPaths.add(resolvedPath);
            accepted.push({
              videoPath: resolvedPath,
              name: file.name,
              sizeBytes: file.size,
              importRootPath: item.relativeDir,
            });
          }
        } else {
          // 未反查命中的文件，明确记入 rejected 并在横幅中说明
          rejected.push({
            pathOrName: file.name,
            reason: {
              kind: 'unreadable',
              detail: '未在服务端工作区中定位到物理路径，请确认文件位于工作区目录下',
            },
          });
        }
      }
    }

    const workers = Array.from({ length: Math.min(concurrency, scanEntries.length) }, () => worker());
    await Promise.all(workers);

    return { accepted, rejected };
  }
}
