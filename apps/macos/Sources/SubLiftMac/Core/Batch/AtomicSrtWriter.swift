import Foundation

/// 08104：同卷原子 SRT 写入。
///
/// 在目标同目录写临时文件（`.basename.tmp-UUID`），完成后
/// `FileManager.replaceItemAt` 原子移动/替换；失败时旧文件字节不变
/// 且临时文件被清理（defer）。
enum AtomicSrtWriter {

    static func write(_ srt: String, to targetURL: URL) throws {
        let data = Data(srt.utf8)
        let dir = targetURL.deletingLastPathComponent()
        let tmp = dir.appendingPathComponent(".\(targetURL.lastPathComponent).tmp-\(UUID().uuidString)")

        do {
            try data.write(to: tmp, options: [.atomic])
        } catch {
            // 临时文件写入失败：清理并上抛（原目标不变）。
            try? FileManager.default.removeItem(at: tmp)
            throw error
        }

        // 无论替换是否成功都清理临时文件（replaceItemAt 成功会移动临时文件，失败则残留）。
        var moved = false
        defer {
            if !moved {
                try? FileManager.default.removeItem(at: tmp)
            }
        }

        do {
            if FileManager.default.fileExists(atPath: targetURL.path) {
                _ = try FileManager.default.replaceItemAt(targetURL, withItemAt: tmp)
            } else {
                try FileManager.default.moveItem(at: tmp, to: targetURL)
            }
            moved = true
        } catch {
            throw error
        }
    }
}
