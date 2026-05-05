// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "media_kit_libs_ios_video",
    platforms: [
        .iOS("13.0")
    ],
    products: [
        .library(name: "media_kit_libs_ios_video", targets: ["media_kit_libs_ios_video", "Mpv"])
    ],
    dependencies: [],
    targets: [
        .binaryTarget(
            name: "Mpv",
            url: "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_ios-universal-video-default.zip",
            checksum: "9984243cd9f22652239530aa250c9e026781b1701d808156d0495b0031af1513"
        ),
        .target(
            name: "media_kit_libs_ios_video",
            dependencies: [
                .target(name: "Mpv")
            ],
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
