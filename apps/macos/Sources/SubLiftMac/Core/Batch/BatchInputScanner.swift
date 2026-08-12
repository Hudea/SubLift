import Foundation

/// 08103：结构化拒绝原因。
enum BatchInputRejectionReason: Equatable, Sendable {
    case unsupportedFormat(String)
    case mkvRequiresFfmpeg
    case unreadable
    case duplicate
    case emptyDirectory
}

/// 08103：单条拒绝记录。
struct BatchScanRejection: Equatable, Sendable {
    let url: URL
    let reason: BatchInputRejectionReason
}

/// 08103：扫描摘要（一次性返回，不逐文件向 UI 同步）。
/// 08309：skipped 记录静默排除项（hidden/package/symlink）计数——B02 摘要三类别。
struct BatchScanSummary: Equatable, Sendable {
    let accepted: [URL]
    let skipped: Int
    let rejected: [BatchScanRejection]
}

/// 08103：统一文件/文件夹输入扫描服务。
///
/// - 复用 `VideoImportPolicy`（MP4/MOV/MKV 大小写扩展名与 MKV-ffmpeg fail-closed）。
/// - 默认不递归；显式递归后按相对路径稳定排序。
/// - 隐藏项、package、symlink 不进入扫描（静默排除）。
/// - 去重键为文件系统 inode（大小写不敏感卷上不同拼写的同一文件也去重）；
///   本批与已有队列双重去重。
/// - 不可读输入/目录枚举失败 → `.unreadable`；确实为空的目录 → `.emptyDirectory`。
/// - 不启动 metadata、Worker 或 OCR。
/// - @unchecked Sendable：VideoImportPolicy 持有非 @Sendable 检测闭包（Swift 5 模式 0 warning）。
struct BatchInputScanner: @unchecked Sendable {
    let policy: VideoImportPolicy
    /// 队列已有任务的源 URL（inode 身份去重）。
    let existingURLs: Set<String>

    init(policy: VideoImportPolicy = VideoImportPolicy(), existingURLs: Set<URL> = []) {
        self.policy = policy
        self.existingURLs = Set(existingURLs.map { Self.identityKey($0) })
    }

    func scan(inputs: [URL], recursive: Bool) -> BatchScanSummary {
        var accepted: [URL] = []
        var skipped = 0
        var rejected: [BatchScanRejection] = []
        var seen = existingURLs

        func consider(_ url: URL) {
            let standardized = url.standardizedFileURL
            guard isScanable(standardized) else {
                skipped += 1  // hidden/package/symlink 静默排除（08309：计入跳过）
                return
            }
            guard fileManager.isReadableFile(atPath: standardized.path) else {
                rejected.append(BatchScanRejection(url: standardized, reason: .unreadable))
                return
            }
            let key = identityKey(standardized)
            guard !seen.contains(key) else {
                rejected.append(BatchScanRejection(url: standardized, reason: .duplicate))
                return
            }
            switch policy.validate(url: standardized) {
            case .valid:
                seen.insert(key)
                accepted.append(standardized)
            case .unsupportedFormat(let ext):
                rejected.append(BatchScanRejection(url: standardized, reason: .unsupportedFormat(ext)))
            case .mkvRequiresFfmpeg:
                rejected.append(BatchScanRejection(url: standardized, reason: .mkvRequiresFfmpeg))
            }
        }

        for input in inputs {
            let standardizedInput = input.standardizedFileURL
            var isDirectory: ObjCBool = false
            guard fileManager.fileExists(atPath: standardizedInput.path, isDirectory: &isDirectory) else {
                rejected.append(BatchScanRejection(url: standardizedInput, reason: .unreadable))
                continue
            }
            if isDirectory.boolValue {
                guard isScanable(standardizedInput) else {
                    skipped += 1
                    continue
                }
                guard fileManager.isReadableFile(atPath: standardizedInput.path) else {
                    rejected.append(BatchScanRejection(url: standardizedInput, reason: .unreadable))
                    continue
                }
                guard let files = enumerateFiles(in: standardizedInput, recursive: recursive, root: standardizedInput) else {
                    // 枚举失败（EPERM 等）不是空目录。
                    rejected.append(BatchScanRejection(url: standardizedInput, reason: .unreadable))
                    continue
                }
                if files.isEmpty {
                    rejected.append(BatchScanRejection(url: standardizedInput, reason: .emptyDirectory))
                } else {
                    for file in files {
                        consider(file)
                    }
                }
            } else {
                consider(standardizedInput)
            }
        }

        return BatchScanSummary(accepted: accepted, skipped: skipped, rejected: rejected)
    }

    // MARK: - Private

    private let fileManager = FileManager.default

    /// 文件身份键：inode（systemFileNumber）优先；失败时回落 lowercase path。
    /// 大小写不敏感卷（APFS 默认）上 .MP4/.mp4 同一文件命中同一键。
    private static func identityKey(_ url: URL) -> String {
        let path = url.standardizedFileURL.path
        if let attrs = try? FileManager.default.attributesOfItem(atPath: path),
           let fileNumber = attrs[.systemFileNumber] as? NSNumber {
            return "ino-\(fileNumber)"
        }
        return "path-\(path.lowercased())"
    }

    private func identityKey(_ url: URL) -> String {
        Self.identityKey(url)
    }

    /// hidden/package/symlink 不进入扫描（静默排除）。
    private func isScanable(_ url: URL) -> Bool {
        guard let values = try? url.resourceValues(forKeys: [
            .isHiddenKey, .isPackageKey, .isSymbolicLinkKey,
        ]) else { return false }
        if values.isHidden == true { return false }
        if values.isPackage == true { return false }
        if values.isSymbolicLink == true { return false }
        return true
    }

    /// 枚举目录下文件；默认非递归（仅直接子项），递归时深层遍历；
    /// 结果按相对输入根的路径稳定排序。枚举失败返回 nil（调用方记 unreadable）。
    private func enumerateFiles(in dir: URL, recursive: Bool, root: URL) -> [URL]? {
        guard let items = try? fileManager.contentsOfDirectory(
            at: dir,
            includingPropertiesForKeys: [.isDirectoryKey, .isHiddenKey, .isPackageKey, .isSymbolicLinkKey],
            options: []
        ) else { return nil }

        var files: [URL] = []
        for item in items {
            guard let values = try? item.resourceValues(forKeys: [
                .isDirectoryKey, .isHiddenKey, .isPackageKey, .isSymbolicLinkKey,
            ]) else { continue }
            // 符号链接目录不跟随（防循环）；hidden/package 目录不递归展开；
            // 文件级 hidden/package/symlink 过滤与跳过计数统一由 consider 的
            // isScanable 处理（08309：B02 摘要跳过类别口径）。
            if values.isDirectory == true {
                if recursive, values.isSymbolicLink != true,
                   values.isHidden != true, values.isPackage != true {
                    files.append(contentsOf: enumerateFiles(in: item, recursive: true, root: root) ?? [])
                }
            } else {
                files.append(item)
            }
        }
        return files.sorted { relativePath($0, root: root) < relativePath($1, root: root) }
    }

    private func relativePath(_ url: URL, root: URL) -> String {
        let urlPath = url.standardizedFileURL.path
        let rootPath = root.standardizedFileURL.path
        guard urlPath.hasPrefix(rootPath) else { return urlPath }
        return String(urlPath.dropFirst(rootPath.count))
    }
}
