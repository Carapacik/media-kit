// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "media_kit_libs_macos_audio",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "media_kit_libs_macos_audio", targets: ["media_kit_libs_macos_audio", "Mpv"])
    ],
    dependencies: [],
    targets: [
        .binaryTarget(
            name: "Mpv",
            url: "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_macos-universal-audio-full.zip",
            checksum: "54baa0a2c5f2c1e7c4e9e68042e64de86b083a1d4af8d8ccf3cbacd81acb5dba"
        ),
        .target(
            name: "media_kit_libs_macos_audio",
            dependencies: [
                .target(name: "Mpv")
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
