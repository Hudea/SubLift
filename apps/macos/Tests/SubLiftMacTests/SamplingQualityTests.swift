import Testing
@testable import SubLiftMac

struct SamplingQualityTests {

    @Test
    func displayNamesHideNumericFps() {
        #expect(SamplingQuality.fast.displayName == "快速")
        #expect(SamplingQuality.balanced.displayName == "平衡")
        #expect(SamplingQuality.fine.displayName == "精细")
        for quality in SamplingQuality.allCases {
            #expect(!quality.displayName.contains(String(quality.sampleFps)))
        }
    }

    @Test
    func mapsToRecommendedSampleFps() {
        #expect(SamplingQuality.fast.sampleFps == 5)
        #expect(SamplingQuality.balanced.sampleFps == 8)
        #expect(SamplingQuality.fine.sampleFps == 12)
    }

    @Test
    func threeTiersOnly() {
        #expect(SamplingQuality.allCases.count == 3)
    }
}
