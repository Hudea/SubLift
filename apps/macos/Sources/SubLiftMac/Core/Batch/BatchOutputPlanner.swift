import Foundation

/// 08104：目标已存在时的冲突策略。
enum BatchOutputConflictPolicy: String, Codable, Sendable {
    /// 默认：跳过（不覆盖）。
    case skip
    /// 首个稳定可用 name (N).srt。
    case rename
    /// 替换（必须携带已确认策略——replace 为显式确认结果）。
    case replace
}

/// 08104：冲突结果（携带现有目标 URL）。
enum BatchOutputConflict: Equatable, Sendable {
    case none
    case skipped(existingURL: URL)
    case renamed(from: URL, to: URL)
    case replaced(existingURL: URL)
}

/// 08104：单源输出计划。
struct BatchOutputPlan: Equatable, Sendable {
    let targetURL: URL
    let conflict: BatchOutputConflict
}

enum BatchOutputPlannerError: Error, Equatable, Sendable {
    /// 目标标准化后逃逸公共输出根，或 source 不在 sourceRoot 之下。
    case targetOutsideRoot
    /// 目标目录不可写（不存在或权限不足）。
    case unwritableDirectory
    /// 同批目标碰撞。
    case duplicateTarget
}

/// 08104：纯路径规划（不接队列调度）。
///
/// - sidecar（outputRoot nil）：`source.deletingPathExtension() + .srt` 同目录。
/// - 公共输出根：文件夹输入保持相对 sourceRoot 的目录；独立文件用 basename。
/// - 目标标准化后必须仍在公共根内（`..` 逃逸 fail-closed）。
/// - 不通过 shell/glob/未验证字符串拼接生成目标路径。
struct BatchOutputPlanner: Sendable {
    let outputRoot: URL?
    let conflictPolicy: BatchOutputConflictPolicy

    init(outputRoot: URL? = nil, conflictPolicy: BatchOutputConflictPolicy = .skip) {
        self.outputRoot = outputRoot?.standardizedFileURL
        self.conflictPolicy = conflictPolicy
    }

    /// 规划单个源。公共根模式下 source 不在 sourceRoot 之下（无法保持相对目录）→ fail-closed。
    func plan(source: URL, sourceRoot: URL?) throws -> BatchOutputPlan {
        let baseTarget = try computeBaseTarget(source: source, sourceRoot: sourceRoot)
        let normalizedTarget = baseTarget.standardizedFileURL

        if let outputRoot, !isWithinRoot(normalizedTarget, root: outputRoot) {
            throw BatchOutputPlannerError.targetOutsideRoot
        }
        guard isWritableTargetDirectory(normalizedTarget) else {
            throw BatchOutputPlannerError.unwritableDirectory
        }

        switch conflictPolicy {
        case .skip:
            if fileExists(normalizedTarget) {
                return BatchOutputPlan(targetURL: normalizedTarget, conflict: .skipped(existingURL: normalizedTarget))
            }
            return BatchOutputPlan(targetURL: normalizedTarget, conflict: .none)
        case .rename:
            let original = normalizedTarget
            if !fileExists(normalizedTarget) {
                return BatchOutputPlan(targetURL: normalizedTarget, conflict: .none)
            }
            let renamed = firstAvailableRename(for: normalizedTarget)
            return BatchOutputPlan(
                targetURL: renamed,
                conflict: .renamed(from: original, to: renamed)
            )
        case .replace:
            if fileExists(normalizedTarget) {
                return BatchOutputPlan(targetURL: normalizedTarget, conflict: .replaced(existingURL: normalizedTarget))
            }
            return BatchOutputPlan(targetURL: normalizedTarget, conflict: .none)
        }
    }

    /// 批量规划：检测同批目标碰撞（fail-closed，大小写不敏感折叠）与公共根逃逸。
    func planAll(sources: [URL], sourceRoot: URL?) throws -> [BatchOutputPlan] {
        var seenTargets = Set<String>()
        var plans: [BatchOutputPlan] = []
        for source in sources {
            let plan = try plan(source: source, sourceRoot: sourceRoot)
            // 大小写不敏感卷（APFS 默认）上 Movie.srt 与 movie.srt 是同一文件：折叠去重。
            let key = plan.targetURL.standardizedFileURL.path.lowercased()
            guard !seenTargets.contains(key) else {
                throw BatchOutputPlannerError.duplicateTarget
            }
            seenTargets.insert(key)
            plans.append(plan)
        }
        return plans
    }

    // MARK: - Private

    private let fileManager = FileManager.default

    private func computeBaseTarget(source: URL, sourceRoot: URL?) throws -> URL {
        let base = source.deletingPathExtension().appendingPathExtension("srt")
        guard let outputRoot else { return base }  // sidecar

        if let sourceRoot {
            // 文件夹输入：保持相对 sourceRoot 的目录。
            // source 必须在 sourceRoot 之下，否则无法保持相对目录（fail-closed）。
            let sourceDir = source.deletingLastPathComponent().standardizedFileURL
            let rootNorm = sourceRoot.standardizedFileURL
            let sourceDirPath = sourceDir.path
            let rootPath = rootNorm.path
            guard sourceDirPath == rootPath || sourceDirPath.hasPrefix(rootPath + "/") else {
                throw BatchOutputPlannerError.targetOutsideRoot
            }
            let relativeDir = sourceDirPath.replacingOccurrences(of: rootPath, with: "")
            return outputRoot
                .appendingPathComponent(relativeDir, isDirectory: true)
                .appendingPathComponent(base.lastPathComponent)
        }
        // 独立文件：basename。
        return outputRoot.appendingPathComponent(base.lastPathComponent)
    }

    private func isWithinRoot(_ target: URL, root: URL) -> Bool {
        let targetPath = target.path
        let rootPath = root.path
        return targetPath == rootPath || targetPath.hasPrefix(rootPath + "/")
    }

    private func fileExists(_ url: URL) -> Bool {
        fileManager.fileExists(atPath: url.path)
    }

    /// 目标目录可写：目录存在且可写，或最近存在的祖先可写（新建路径）。
    private func isWritableTargetDirectory(_ target: URL) -> Bool {
        let dir = target.deletingLastPathComponent()
        var probe = dir
        while !fileManager.fileExists(atPath: probe.path) {
            let parent = probe.deletingLastPathComponent()
            guard parent.path != probe.path else { break }
            probe = parent
        }
        return fileManager.isWritableFile(atPath: probe.path)
    }

    /// 首个稳定可用 `name (N).srt`。
    private func firstAvailableRename(for target: URL) -> URL {
        let dir = target.deletingLastPathComponent()
        let base = target.deletingPathExtension().lastPathComponent
        let ext = target.pathExtension.isEmpty ? "srt" : target.pathExtension
        var n = 1
        while true {
            let candidate = dir.appendingPathComponent("\(base) (\(n)).\(ext)")
            if !fileExists(candidate) {
                return candidate
            }
            n += 1
        }
    }
}
