import SwiftUI

struct SettingsView: View {
    @AppStorage("default_engine") var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") var samplingQuality: SamplingQuality = .fast

    var body: some View {
        Form {
            Picker("OCR 引擎", selection: $defaultEngine) {
                Text("Apple Vision (推荐)").tag(OcrEngineName.vision)
                Text("PaddleOCR (跨平台)").tag(OcrEngineName.paddle)
                Text("Mock (测试)").tag(OcrEngineName.mock)
            }
            .pickerStyle(.menu)

            Picker("采样密度", selection: $samplingQuality) {
                ForEach(SamplingQuality.allCases) { quality in
                    Text(quality.displayName).tag(quality)
                }
            }
            .pickerStyle(.segmented)
            .help(samplingQuality.helpText)

            Text(samplingQuality.helpText)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(40)
        .frame(width: 420, height: 200)
    }
}
