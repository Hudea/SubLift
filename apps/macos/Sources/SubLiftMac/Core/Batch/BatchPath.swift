import Foundation

/// 08511：路径相对化辅助。
///
/// 禁止用字符串全局替换做路径算术（避免 `/data/v` 把 `/data/v/show/data/v`
/// 里的后续同名分量一起替换）。
enum BatchPath {
    /// 相对路径，无前导 `/`。`url` 不在 `root` 下时返回 nil。
    /// 比较用 standardizedFileURL。
    static func relativePath(from root: URL, to url: URL) -> String? {
        let urlPath = url.standardizedFileURL.path
        let rootPath = root.standardizedFileURL.path
        guard urlPath == rootPath || urlPath.hasPrefix(rootPath + "/") else { return nil }
        let raw = String(urlPath.dropFirst(rootPath.count))
        return raw.hasPrefix("/") ? String(raw.dropFirst()) : raw
    }
}
