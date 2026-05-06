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

let libmpvArtifactBase = "https://github.com/media-kit/libmpv-darwin-build/releases/download/v0.6.3/libmpv-xcframeworks_v0.6.3_macos-universal-video-default"
let libmpvChecksums = [
    "Ass": "f75d811c6ebd4fa6fd70a0df73b4d252422e082c1f232b07cb679a0dc852f756",
    "Avcodec": "56e9c08ac2a3a8a8c4fa8d3ac6aef2a915fafe8f1e8cbd00add19eae1a05b6e1",
    "Avfilter": "c86fa7ec909cd987fe68dd58c9546672b63d4105740fb6f639fe9464f396132f",
    "Avformat": "6c99bbc745b0766bb9cbc09a1310739274000c6ba8cf8493aad9b7d6d83f95b1",
    "Avutil": "575ee0fe22741f392d4bb2b61e00fdb8e1f94a6f74c6291835a4fe072f3f68ab",
    "Dav1d": "e91ac28331375bdb81967f9dd7f31bde414deac071e29d8de6f2c86eb5a23b14",
    "Freetype": "5c092dd4a915fa9ddb9ec97af367d571950ddbe0d8d2477725da13b306130bca",
    "Fribidi": "75b37e93620e6da0533bf79b0493c3e1100e00b557a1f68004a3190c24256de5",
    "Harfbuzz": "c62a7314615c0ce63c9599f23a69688e060c523944a505a76efcd3c9e11e6a37",
    "Mbedcrypto": "8b75ffa8012484eff36d0d40cf807c33b438331bf08da96e3f53d2db3a087822",
    "Mbedtls": "181aee351f605e2f14e8cbbf0b828abe971fa6a4ff766d92a329472e55e75459",
    "Mbedx509": "a23fb53e2fcfac6c736e111c4f86f8985cf96fbd2e4b93df48ab4437a84c8947",
    "Mpv": "88228d62e0874b3aa95a027b615d752985ee78bf65a30f155d049a37a0d72614",
    "Png16": "eda83fa9937ec28d3e2019fd3c5c91413cb79cd58f2fbf0cae980dfd23894826",
    "Swresample": "b585e700614993ad84dcb28dbf659b568b0f59d7f1e873832267edbec933f034",
    "Swscale": "64dcc76b473f2c8bc78cb92a20200185f7d050c76167003b3f40ab14c5aef08a",
    "Uchardet": "0d274571a8403331de97c9141b5c8f3d0332cfef2df3fb0a428e8e574968c7e9",
    "Xml2": "e002dd097cbcd1cc39718ed690cb335f38b886998d2878f4a264fef82ae7b611"
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
