import Foundation

/// 10311：引擎可见性纯逻辑（capability 检测注入、真实状态展示）。
enum EngineCapability {

    /// 非开发环境的确定性默认值。Composition root 在启动任务前也应使用
    /// `normalizedSelection(_:developerMode:)`，避免绕过 Settings UI 的陈旧偏好。
    static let defaultProductionEngine: OcrEngineName = .vision

    struct Status: Equatable {
        let available: Bool
        let summary: String
        let detail: String?
    }

    /// 可见引擎列表：Mock 仅开发者模式（Automatic/Whisper 从不出现）。
    static func visibleEngines(developerMode: Bool) -> [OcrEngineName] {
        developerMode ? [.vision, .paddle, .mock] : [.vision, .paddle]
    }

    /// 将持久化选择限制到当前可见（亦即当前允许使用）的引擎集合。
    ///
    /// 用户可能先在开发者模式选择 Mock，再关闭开发者模式。此时旧偏好仍是合法枚举值，
    /// 但已经不再是合法产品选择；必须回落到 Vision，避免 Picker 空白及 Mock 泄漏到任务启动。
    static func normalizedSelection(
        _ selection: OcrEngineName,
        developerMode: Bool
    ) -> OcrEngineName {
        let visible = visibleEngines(developerMode: developerMode)
        if visible.contains(selection) {
            return selection
        }
        if visible.contains(defaultProductionEngine) {
            return defaultProductionEngine
        }
        return visible.first ?? defaultProductionEngine
    }

    /// 引擎 capability 状态（真实检测结果注入；缺失如实 inline 说明，无假下载按钮）。
    static func status(
        for engine: OcrEngineName,
        ffmpegAvailable: Bool,
        paddleModelsAvailable: Bool,
        developerMode: Bool
    ) -> Status {
        switch engine {
        case .vision:
            return Status(
                available: true,
                summary: "可用",
                detail: ffmpegAvailable
                    ? nil
                    : "ffmpeg 未安装：MKV 导入与回退预览不可用"
            )
        case .paddle:
            if paddleModelsAvailable {
                return Status(available: true, summary: "可用", detail: nil)
            }
            return Status(
                available: false,
                summary: "不可用",
                detail: "PaddleOCR 模型未安装（~/.cache/sublift/rapidocr-models）"
            )
        case .mock:
            if developerMode {
                return Status(
                    available: true,
                    summary: "可用（测试）",
                    detail: "Mock 引擎仅用于测试"
                )
            }
            return Status(
                available: false,
                summary: "仅开发者模式",
                detail: nil
            )
        }
    }

    /// 引擎实际 runtime（产品只有 Native；缺失 capability 时如实不可用）。
    static func runtimeLabel(
        for engine: OcrEngineName,
        ffmpegAvailable: Bool,
        paddleModelsAvailable: Bool
    ) -> String {
        do {
            _ = try RuntimePolicy.resolve(
                requestedEngine: engine.rawValue,
                isCppPaddleAvailable: paddleModelsAvailable
            )
            return "C++ runtime"
        } catch {
            return "不可用（\(error.localizedDescription)）"
        }
    }
}

/// 10311：Settings 面板展示纯逻辑。
enum SettingsPresentation {
    static let sections = ["General", "Recognition", "Advanced"]
    /// 面板所有可执行动作（用于断言无假下载按钮）。
    static let actions: [String] = []
}
