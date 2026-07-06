// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SubLiftMac",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "SubLiftMac",
            path: "Sources/SubLiftMac"
        ),
        .testTarget(
            name: "SubLiftMacTests",
            dependencies: ["SubLiftMac"],
            path: "Tests/SubLiftMacTests"
        ),
    ]
)
