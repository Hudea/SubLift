import Foundation

/// 10415：单次提取任务的运行配置快照（engine + quality）。
///
/// 用于表达"配置生效状态"：任务启动时按当时偏好冻结一份 active 快照，
/// 成功后转存为 final 快照；偏好变化不会追溯改写运行中或已完成的配置。
/// 08102：增加最小 Codable（批量任务持久化复用同一真源，不复制枚举字符串）。
struct ExtractionConfiguration: Equatable, Sendable, Codable {
    let engine: OcrEngineName
    let quality: SamplingQuality
}
