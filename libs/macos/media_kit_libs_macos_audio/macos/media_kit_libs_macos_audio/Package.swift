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

let libmpvArtifactBase = "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_macos-universal-audio-full"
let libmpvChecksums = [
    "Avcodec": "acbadb681445a59869b6bbc76429feb622c719927877df8f38fb87b0c81ec508",
    "Avfilter": "9dcddd406d92f7da5955c0b8afa52548e033227160d6354b62a204cfde8173ce",
    "Avformat": "7c5bcdac65cda3ff4aae8564c2ca33a34dc146de0e6ed08dbcff94d72627295c",
    "Avutil": "e5c6caff1efdc8554276396818d5a66484b612bbb0ff50186342dc4f3cae3286",
    "Mbedcrypto": "135e3eb8ae964cfbe3241a6138d387fb16ff95ba2fe42e2a0ad461d453c0c5a6",
    "Mbedtls": "1275ad674dff09340a916905685126c30e2dacb6428b1662bc49a16b7da127c5",
    "Mbedx509": "2cfd4c7ea9076948616a7d2a7bddc0d44b61b57179f8692f3c4cc117393345b8",
    "Mpv": "b486ae8f84448ecd8e2e98b166247d962bed3f22c2203d3b133b62d19b373d2f",
    "Swresample": "541533d5d32d652e658ab2b6b211d67dac93d55b706b0fd5d3c6801189b965b4",
    "Swscale": "33c19c2e9f2a4331100a0464a70da6422eedce1bd80f02ce391880eb5171eb55"
]

let package = Package(
    name: "media_kit_libs_macos_audio",
    platforms: [
        .macOS("10.15")
    ],
    products: [
        .library(name: "media_kit_libs_macos_audio", targets: ["media_kit_libs_macos_audio"] + libmpvTargets)
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
            name: "media_kit_libs_macos_audio",
            dependencies: libmpvTargets.map { framework in .target(name: framework) },
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
