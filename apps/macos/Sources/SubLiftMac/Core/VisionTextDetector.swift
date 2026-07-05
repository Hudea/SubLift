import CoreGraphics
import Foundation
import Vision

/// feat-022：用 Apple Vision 检测帧内文字候选框。
enum VisionTextDetector {

    struct Detection: Equatable {
        let normalizedRect: CGRect
        let text: String
        let confidence: Float
    }

    enum DetectError: Error, Equatable {
        case performFailed(String)
        case noResults
    }

    /// 对单帧图像执行文字检测。
    static func detect(
        in cgImage: CGImage,
        languages: [String] = ["zh-Hans", "en-US"]
    ) throws -> [Detection] {
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = .accurate
        request.usesLanguageCorrection = false
        request.recognitionLanguages = languages

        let handler = VNImageRequestHandler(cgImage: cgImage, options: [:])
        do {
            try handler.perform([request])
        } catch {
            throw DetectError.performFailed(error.localizedDescription)
        }

        guard let observations = request.results, !observations.isEmpty else {
            throw DetectError.noResults
        }

        var detections: [Detection] = []
        detections.reserveCapacity(observations.count)

        for observation in observations {
            guard let candidate = observation.topCandidates(1).first else { continue }
            let confidence = candidate.confidence
            let text = candidate.string.trimmingCharacters(in: .whitespacesAndNewlines)
            detections.append(
                Detection(
                    normalizedRect: observation.boundingBox,
                    text: text,
                    confidence: confidence
                )
            )
        }

        if detections.isEmpty {
            throw DetectError.noResults
        }

        // 按垂直位置从上到下排序，编号更稳定。
        detections.sort { lhs, rhs in
            let lhsY = 1.0 - lhs.normalizedRect.maxY
            let rhsY = 1.0 - rhs.normalizedRect.maxY
            if lhsY == rhsY {
                return lhs.normalizedRect.minX < rhs.normalizedRect.minX
            }
            return lhsY < rhsY
        }

        return detections
    }
}