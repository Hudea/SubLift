import Foundation

/// 10311：引擎可见性纯逻辑（capability 检测注入、真实状态展示）。
enum EngineCapability {

    struct Status: Equatable {
        let available: Bool
        let summary: String
        let detail: String?
    }

    /// 可见引擎列表：Mock 仅开发者模式（Automatic/Whisper 从不出现）。
    static func visibleEngines(developerMode: Bool) -> [OcrEngineName] {
        developerMode ? [.vision, .paddle, .mock] : [.vision, .paddle]
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

    /// 引擎实际 runtime（RuntimePolicy 真实解析；Python 仅显式 Oracle/回滚）。
    static func runtimeLabel(
        for engine: OcrEngineName,
        ffmpegAvailable: Bool,
        paddleModelsAvailable: Bool
    ) -> String {
        do {
            let choice = try RuntimePolicy.resolve(
                requestedEngine: engine.rawValue,
                isCppPaddleAvailable: paddleModelsAvailable
            )
            switch choice.runtime {
            case .cpp:
                return "C++ runtime（默认）"
            case .python:
                return "Python Oracle/回滚（显式）"
            }
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
