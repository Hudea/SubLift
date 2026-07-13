import SwiftUI

struct SettingsView: View {
    @AppStorage("default_engine") var defaultEngine: OcrEngineName = .vision

    var body: some View {
        Form {
            Picker("OCR 引擎", selection: $defaultEngine) {
                Text("Apple Vision (推荐)").tag(OcrEngineName.vision)
                Text("Mock (测试)").tag(OcrEngineName.mock)
            }
            .pickerStyle(.menu)
        }
        .padding(40)
        .frame(width: 400, height: 140)
    }
}
