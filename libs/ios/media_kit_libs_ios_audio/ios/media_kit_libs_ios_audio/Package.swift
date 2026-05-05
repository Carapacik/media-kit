// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "media_kit_libs_ios_audio",
    platforms: [
        .iOS("13.0")
    ],
    products: [
        .library(name: "media_kit_libs_ios_audio", targets: ["media_kit_libs_ios_audio", "Mpv"])
    ],
    dependencies: [],
    targets: [
        .binaryTarget(
            name: "Mpv",
            url: "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_ios-universal-audio-default.zip",
            checksum: "6c08b6364e12d3c9a0c584941b2192cb5a25bcbd33cfaff16402614be797826c"
        ),
        .target(
            name: "media_kit_libs_ios_audio",
            dependencies: [
                .target(name: "Mpv")
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
