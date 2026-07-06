import SwiftUI

struct SettingsView: View {
    @AppStorage("default_engine") var defaultEngine: OcrEngineName = .vision
    @AppStorage("enable_ssim_patrol") var enableSsimPatrol: Bool = true

    var body: some View {
        Form {
            Picker("OCR 引擎", selection: $defaultEngine) {
                Text("Apple Vision (推荐)").tag(OcrEngineName.vision)
                Text("Mock (测试)").tag(OcrEngineName.mock)
            }
            .pickerStyle(.menu)

            Toggle("SSIM 巡逻 (提升连续字幕分段)", isOn: $enableSsimPatrol)
                .help("启用后补强 dHash 对中文短句的漏检，打轴 F1 提升约 15pp。增加少量计算开销。")
        }
        .padding(40)
        .frame(width: 400, height: 180)
    }
}
