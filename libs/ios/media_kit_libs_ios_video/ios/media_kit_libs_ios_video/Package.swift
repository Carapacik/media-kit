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

let libmpvArtifactBase = "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_ios-universal-video-default"
let libmpvChecksums = [
    "Ass": "f1f28715c11017a2d85b5ceefc34db9a81026ad5f93ce55958f015739cf0a8f5",
    "Avcodec": "956087021c4d47630f5a787bb220bbecf1db8a3973274b86847fef424d182629",
    "Avfilter": "ffd5b05b8690fe9baa954228bf23c0299c382d3e0eb7052430e039b376d4d9b6",
    "Avformat": "8963b522ed8462ab055d867ff38c949480d959f922150c0ed756e340e36f5266",
    "Avutil": "e11b83570d305d3ec4816b4617ba0d9ba3e94d33fff17482bb502f5c677d9b05",
    "Dav1d": "f676271d6e501c261e93672d9a5532b7d72d59e16e7873e41202f317c6a3751c",
    "Freetype": "d933881734fae032015a1fdafb52cdafa9a48ad408a17eafc7ec882d876ed975",
    "Fribidi": "768f7a864f332087df82473bd3a8bfaa14e8c8be070f8b0f6d6f88eef2d5a440",
    "Harfbuzz": "20c87a44c35b4fe1973b184912f4d1668ca9975a8ac4e387a47a7e9e36b2fceb",
    "Mbedcrypto": "cef7ff4dcf2dedaace0d7b6a80bc789f9fb9bfb8e416d288399362090a8d55b0",
    "Mbedtls": "3925ca261f8fb8a83391c9ad44e17ad56e2f02485245f124e071dabbc06fdd88",
    "Mbedx509": "b64221a114717495eac29a783b818c76d79088ef90d629e6cef67e18260784ea",
    "Mpv": "60c21a9aadfea8ef45fc56de99efe4d03c15b7e63c5cd6bd2f92d45768802cde",
    "Png16": "01cbe6edff6928d91b46fa8861b70b7f8f4056152af15301bcef1eec92c7b75a",
    "Swresample": "73faeb62f1041a455bf5f23ea77ad0968e921e888557588871e6c08141fd4a60",
    "Swscale": "94d7578f6ba1a340fa181d2778f10b5b56437aa1edd9054d2cbf9dabc6e645b5",
    "Uchardet": "4270a2fe35b01b64f45d96ebc25a464d6f5825f9a1c47ce39bfba584fae30385",
    "Xml2": "6ef17e0e3ff721bab3b8aae6f26443568a9f3d7041726cbf1d330b08c3c25891"
]

let package = Package(
    name: "media_kit_libs_ios_video",
    platforms: [
        .iOS("13.0")
    ],
    products: [
        .library(name: "media_kit_libs_ios_video", targets: ["media_kit_libs_ios_video"] + libmpvTargets)
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
            name: "media_kit_libs_ios_video",
            dependencies: libmpvTargets.map { framework in .target(name: framework) },
            resources: [
                .process("PrivacyInfo.xcprivacy")
            ]
        )
    ]
)
