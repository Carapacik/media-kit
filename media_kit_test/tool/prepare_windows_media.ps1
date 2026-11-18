param(
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../build/test-media')
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$fixtureRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

function New-Fixture {
    param([string]$Name, [string]$Size, [int]$Rate, [int]$Seconds,
          [string]$Codec, [string]$PixelFormat)
    $target = Join-Path $fixtureRoot $Name
    if (Test-Path -LiteralPath $target) {
        Write-Host "Keeping existing $target"
        return
    }
    & ffmpeg -hide_banner -loglevel error -n -f lavfi `
        -i "testsrc2=size=${Size}:rate=${Rate}" -t $Seconds `
        -c:v $Codec -preset ultrafast -pix_fmt $PixelFormat `
        -movflags +faststart $target
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg failed for $Name" }
}

New-Fixture landscape.mp4 1280x720 30 8 libx264 yuv420p
New-Fixture portrait.mp4 1080x1920 30 8 libx264 yuv420p
New-Fixture wide.mp4 3840x1600 24 6 libx264 yuv420p
New-Fixture hevc-padded.mp4 720x576 25 8 libx265 yuv420p
New-Fixture hevc-main10.mp4 1080x1920 25 8 libx265 yuv420p10le
