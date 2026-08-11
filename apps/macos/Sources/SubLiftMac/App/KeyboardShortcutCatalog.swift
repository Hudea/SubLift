import Foundation

/// 10412：键盘路径目录（A01 数据真源）。
///
/// 与 WorkspaceCommands / WorkspaceRootView 的实际快捷键保持一致；
/// 测试断言无重复与关键动作存在，防止快捷键被意外移除或冲突。
enum KeyboardShortcutCatalog {

    struct Shortcut: Equatable {
        let key: String
        let modifiers: String
        let action: String
    }

    static let entries: [Shortcut] = [
        Shortcut(key: "o", modifiers: "⌘", action: "打开视频"),
        Shortcut(key: "e", modifiers: "⇧⌘", action: "导出 SRT"),
        Shortcut(key: "s", modifiers: "⌥⌘", action: "拆分字幕"),
        Shortcut(key: "m", modifiers: "⌥⌘", action: "与下一条合并"),
        Shortcut(key: " ", modifiers: "", action: "播放/暂停"),
        Shortcut(key: "r", modifiers: "⌥⌘", action: "编辑字幕区域"),
        Shortcut(key: "i", modifiers: "⌘", action: "显示 Inspector"),
        Shortcut(key: "esc", modifiers: "", action: "退出区域编辑"),
    ]
}
