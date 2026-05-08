// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let libmpvTargets = [
    "Ass",
    "Avcodec",
    "Avfilter",
    "Avformat",
    "Avutil",
    "Dav1d",
    "Freetype",
    "Fribidi",
    "Harfbuzz",
    "Mbedcrypto",
    "Mbedtls",
    "Mbedx509",
    "Mpv",
    "Png16",
    "Swresample",
    "Swscale",
    "Uchardet",
    "Xml2"
]

let libmpvArtifactBase = "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.7.0/libmpv-xcframeworks_v0.7.0_macos-universal-video-default"
let libmpvChecksums = [
    "Ass": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Avcodec": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Avfilter": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Avformat": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Avutil": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Dav1d": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Freetype": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Fribidi": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Harfbuzz": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Mbedcrypto": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Mbedtls": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Mbedx509": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Mpv": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Png16": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Swresample": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Swscale": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Uchardet": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa",
    "Xml2": "aaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aaaaa111aa"
]

let package = Package(
    name: "media_kit_libs_macos_video",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "media_kit_libs_macos_video", targets: ["media_kit_libs_macos_video"] + libmpvTargets)
    ],
    dependencies: [],
    targets: libmpvTargets.map { framework in
        .binaryTarget(
            name: framework,
            url: "\(libmpvArtifactBase)_\(framework).zip",
            checksum: libmpvChecksums[framework]!
        )
    } + [
        .target(
            name: "media_kit_libs_macos_video",
            dependencies: libmpvTargets.map { framework in .target(name: framework) },
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
