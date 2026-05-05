// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "media_kit_libs_macos_video",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "media_kit_libs_macos_video", targets: ["media_kit_libs_macos_video", "Mpv"])
    ],
    dependencies: [],
    targets: [
        .binaryTarget(
            name: "Mpv",
            url: "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_macos-universal-video-default.zip",
            checksum: "e26feb7226e1146fb04024058c6c1967e1e9f9c630e580d5075393f19fa669cc"
        ),
        .target(
            name: "media_kit_libs_macos_video",
            dependencies: [
                .target(name: "Mpv")
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
