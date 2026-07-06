// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "avf-spike",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "avf-spike", path: "Sources/avf-spike"),
    ]
)
