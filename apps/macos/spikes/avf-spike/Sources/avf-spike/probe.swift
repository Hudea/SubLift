import AVFoundation
import CoreMedia
import Foundation

@main
struct AVFProbe {
    static func main() {
        let args = CommandLine.arguments
        guard args.count >= 2 else {
            FileHandle.standardError.write("usage: avf-spike <video_path> [<video_path>...]\n".data(using: .utf8)!)
            exit(2)
        }
        print("PATH\tREADABLE\tERR_CODE\tERR_DOMAIN\tW\tH\tCODEC\tDUR_MS\tELAPSED_MS")
        for path in args.dropFirst() {
            probe(path)
        }
    }

    static func probe(_ path: String) {
        let url = URL(fileURLWithPath: path)
        let start = DispatchTime.now()

        var readable = false
        var errCode = "-"
        var errDomain = "-"
        var w = "-"
        var h = "-"
        var codec = "-"
        var durMs = "-"

        let asset = AVURLAsset(url: url)
        let gen = AVAssetImageGenerator(asset: asset)
        gen.appliesPreferredTrackTransform = true
        let sem = DispatchSemaphore(value: 0)

        gen.generateCGImagesAsynchronously(forTimes: [NSValue(time: CMTime.zero)]) { _, image, _, result, error in
            if let image = image {
                readable = true
                w = String(image.width)
                h = String(image.height)
                let dur = asset.duration
                let s = CMTimeGetSeconds(dur)
                if s.isFinite { durMs = String(Int(s * 1000)) }
                if let track = asset.tracks(withMediaType: .video).first,
                   let raw = track.formatDescriptions.first {
                    let fmt = raw as! CMFormatDescription
                    let fourCC = CMFormatDescriptionGetMediaSubType(fmt)
                    codec = fourCharCodeToString(fourCC)
                }
            } else if let e = error as NSError? {
                errCode = String(e.code)
                errDomain = e.domain
            } else {
                errCode = String(result.rawValue)
                errDomain = "ImageGenResult"
            }
            sem.signal()
        }
        sem.wait()

        let elapsedMs = Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1_000_000.0
        let row = [
            path,
            readable ? "true" : "false",
            errCode,
            errDomain,
            w, h,
            codec,
            durMs,
            String(format: "%.0f", elapsedMs),
        ].joined(separator: "\t")
        print(row)
    }

    static func fourCharCodeToString(_ code: FourCharCode) -> String {
        let bytes: [UInt8] = [
            UInt8((code >> 24) & 0xFF),
            UInt8((code >> 16) & 0xFF),
            UInt8((code >> 8) & 0xFF),
            UInt8(code & 0xFF),
        ]
        return String(bytes: bytes, encoding: .ascii) ?? String(format: "%08x", code)
    }
}
