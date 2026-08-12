import XCTest
@testable import SubLiftMac

/// 10414：Welcome 核心层级聚合、底部隐私说明独立弱化。
final class WelcomeLayoutTests: XCTestCase {

    func testCoreContentUsesOneCohesiveGroup() {
        XCTAssertEqual(WelcomeLayout.coreGroupSpacing, 24)
        XCTAssertLessThanOrEqual(WelcomeLayout.actionGroupSpacing, 14)
        XCTAssertEqual(WelcomeLayout.contentMaxWidth, 520)
    }

    func testCompactHeightRetainsUsableVerticalInsets() {
        XCTAssertEqual(WelcomeLayout.verticalInset(forHeight: 600), 24)
        XCTAssertEqual(WelcomeLayout.verticalInset(forHeight: 800), 40)
    }
}
