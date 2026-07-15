import CoreGraphics
import Testing
@testable import SubLiftMac

struct PreviewLayoutTests {

    @Test
    func keepsLandscapeVideoAtItsAspectFitHeight() {
        let height = PreviewLayout.height(
            containerSize: CGSize(width: 400, height: 700),
            videoSize: CGSize(width: 1920, height: 1080)
        )

        #expect(height == 225)
    }

    @Test
    func reservesVerticalSpaceForControlsInShortWindow() {
        let height = PreviewLayout.height(
            containerSize: CGSize(width: 700, height: 480),
            videoSize: CGSize(width: 1920, height: 1080)
        )

        #expect(height == 240)
    }

    @Test
    func capsTallPreviewAndFallsBackToStandardAspectRatio() {
        let capped = PreviewLayout.height(
            containerSize: CGSize(width: 1000, height: 1000),
            videoSize: CGSize(width: 1920, height: 1080)
        )
        let fallback = PreviewLayout.height(
            containerSize: CGSize(width: 320, height: 700),
            videoSize: .zero
        )

        #expect(capped == 420)
        #expect(fallback == 180)
    }
}
