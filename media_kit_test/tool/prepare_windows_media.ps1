param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/test-media')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$fixtureRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

function New-Fixture {
    param([string]$Name, [string]$Size, [int]$Rate, [int]$Seconds,
          [string]$Codec, [string]$PixelFormat, [string]$Filter = '')
    $target = Join-Path $fixtureRoot $Name
    if (Test-Path -LiteralPath $target) {
        Write-Host "Keeping existing $target"
        return
    }
    $sourceFilter = if ($Filter) { $Filter } else { "testsrc2=size=${Size}:rate=${Rate}" }
    & ffmpeg -hide_banner -loglevel error -n -f lavfi `
        -i $sourceFilter -t $Seconds `
        -c:v $Codec -preset ultrafast -pix_fmt $PixelFormat `
        -movflags +faststart $target
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed for $Name" }
}

New-Fixture landscape.mp4 1280x720 30 8 libx264 yuv420p
New-Fixture portrait.mp4 1080x1920 30 8 libx264 yuv420p
New-Fixture wide.mp4 3840x1600 24 6 libx264 yuv420p
New-Fixture hevc-padded.mp4 720x576 25 8 libx265 yuv420p
New-Fixture hevc-main10.mp4 1080x1920 25 8 libx265 yuv420p10le
New-Fixture aspect-h264.mp4 1600x900 15 5 libx264 yuv420p `
    'color=c=red:size=1600x900:rate=15,drawbox=x=700:y=350:w=200:h=200:color=white:t=fill'
New-Fixture aspect-hevc.mp4 1600x900 15 5 libx265 yuv420p `
    'color=c=red:size=1600x900:rate=15,drawbox=x=700:y=350:w=200:h=200:color=white:t=fill'

$rotated = Join-Path $fixtureRoot 'landscape-rotated.mp4'
if (!(Test-Path -LiteralPath $rotated)) {
    & ffmpeg -hide_banner -loglevel error -n -display_rotation 90 `
        -i (Join-Path $fixtureRoot 'landscape.mp4') -c copy $rotated
    if ($LASTEXITCODE -ne 0) { throw 'FFmpeg failed for rotated fixture' }
}
$rotation = (& ffprobe -v error -select_streams v:0 -show_entries stream_side_data=rotation `
    -of json $rotated | ConvertFrom-Json).streams[0].side_data_list[0].rotation
if ($rotation -ne 90) { throw "Fixture $rotated must contain 90-degree rotation metadata" }

$quadrants = 'color=c=red:size=320x180:rate=15,drawbox=x=160:y=0:w=160:h=90:color=lime:t=fill,drawbox=x=0:y=90:w=160:h=90:color=blue:t=fill,drawbox=x=160:y=90:w=160:h=90:color=yellow:t=fill'
New-Fixture rotation-h264-0.mp4 320x180 15 4 libx264 yuv420p $quadrants
New-Fixture rotation-hevc-0.mp4 320x180 15 4 libx265 yuv420p $quadrants
foreach ($codec in @('h264', 'hevc')) {
    foreach ($degrees in @(90, 180, 270)) {
        $target = Join-Path $fixtureRoot "rotation-$codec-$degrees.mp4"
        if (!(Test-Path -LiteralPath $target)) {
            & ffmpeg -hide_banner -loglevel error -n -display_rotation $degrees `
                -i (Join-Path $fixtureRoot "rotation-$codec-0.mp4") -c copy $target
            if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed for $target" }
        }
        $actual = (& ffprobe -v error -select_streams v:0 -show_entries stream_side_data=rotation `
            -of json $target | ConvertFrom-Json).streams[0].side_data_list[0].rotation
        if ((($actual % 360) + 360) % 360 -ne $degrees) { throw "Invalid rotation in $target" }
    }
}
