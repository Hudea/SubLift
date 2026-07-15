import Testing
@testable import SubLiftMac

struct ProcessingRateTests {

    @Test
    func convertsProcessedFramesToRealTimeMultiplier() {
        let rate = ProcessingRate.realTimeMultiplier(
            processedFrames: 60,
            sampleFps: 5,
            elapsedSeconds: 3
        )

        #expect(rate == 4)
        #expect(ProcessingRate.displayText(for: rate) == "4.0× 实时")
    }

    @Test
    func suppressesRateBeforeThereIsEnoughSignal() {
        #expect(
            ProcessingRate.realTimeMultiplier(
                processedFrames: 1,
                sampleFps: 5,
                elapsedSeconds: 0.1
            ) == nil
        )
        #expect(
            ProcessingRate.realTimeMultiplier(
                processedFrames: 0,
                sampleFps: 5,
                elapsedSeconds: 1
            ) == nil
        )
    }

    @Test
    func rejectsInvalidInputs() {
        #expect(
            ProcessingRate.realTimeMultiplier(
                processedFrames: 10,
                sampleFps: 0,
                elapsedSeconds: 1
            ) == nil
        )
        #expect(ProcessingRate.displayText(for: .infinity) == nil)
    }
}
