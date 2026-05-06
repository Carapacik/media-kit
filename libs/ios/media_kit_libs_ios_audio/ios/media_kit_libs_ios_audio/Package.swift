// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let libmpvTargets = [
    "Avcodec",
    "Avfilter",
    "Avformat",
    "Avutil",
    "Mbedcrypto",
    "Mbedtls",
    "Mbedx509",
    "Mpv",
    "Swresample",
    "Swscale"
]

let libmpvArtifactBase = "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_ios-universal-audio-default"
let libmpvChecksums = [
    "Avcodec": "03178f0bb5b2dd47eb0ebdf0cb9b4fd8dc91fbe5ba5b4889ad7c9674817d59aa",
    "Avfilter": "bdbc7c6caa00097dac57a21d254dd0dd809b700023b80b6cf89262346a3a9588",
    "Avformat": "bd1cb5f81bb7c7b645ead692c565d10e911347c348a5854989bb26ac21ecef21",
    "Avutil": "fd7aba8c3e64c35c82856cdf1f6565ac91fe41e8b1fbcf28638f0e52eff3f6b4",
    "Mbedcrypto": "ee94d021d18b278ded58e30988ee87ea8af797666147c6b0be306ce159dac679",
    "Mbedtls": "b8ba5f1e48250f98b8fbaf2bd32cc80723079f160f11aa7976857cfc2b10fd7f",
    "Mbedx509": "501a797eed3f7812280f2047aab5532604529cb207b8db382f21d3ad832ec06e",
    "Mpv": "e9c2e96588e2dbf9b5ac32755e2d4cdd124d490b3d3edc7ca7eea6a7842c4df7",
    "Swresample": "0ad4a0f2a8621fce228e04216151c2ac19075621c4ae079540b6d323d62225e4",
    "Swscale": "8cf17654a4d5ad1ceffa7eb0c9eb599e06e39d0603ef9db232bcf463eabb2504"
]

let package = Package(
    name: "media_kit_libs_ios_audio",
    platforms: [
        .iOS("13.0")
    ],
    products: [
        .library(name: "media_kit_libs_ios_audio", targets: ["media_kit_libs_ios_audio"] + libmpvTargets)
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
            name: "media_kit_libs_ios_audio",
            dependencies: libmpvTargets.map { framework in .target(name: framework) },
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
