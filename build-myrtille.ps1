param(
    [string]$CMake = 'cmake',
    [string]$Generator = 'Visual Studio 18 2026',
    [ValidateSet('FFmpeg', 'OpenH264')]
    [string]$H264Decoder = 'FFmpeg',
    [switch]$ConfigureOnly,
    [switch]$SkipConfigure
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$build = Join-Path $source 'build-myrtille'
$deps = Join-Path $source '.deps/installed/x86-windows'
if (!(Test-Path (Join-Path $deps 'include/openssl/ssl.h'))) {
    throw 'Install the pinned x86 dependencies first; see MYRTILLE-MIGRATION.md.'
}
$options = @(
    '-S', $source, '-B', $build, '-G', $Generator, '-A', 'Win32',
    "-DCMAKE_PREFIX_PATH=$deps", "-DCMAKE_INSTALL_PREFIX=$source/artifacts",
    '-DBUILD_SHARED_LIBS=ON', '-DCLIENT_INTERFACE_SHARED=ON',
    '-DWITH_LIBRARY_VERSIONING=OFF', '-DWITH_BINARY_VERSIONING=OFF',
    '-DWITH_CLIENT_WINDOWS=ON', '-DWITH_CLIENT_SDL=OFF',
    '-DCHANNEL_URBDRC=OFF', '-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON',
    '-DWITH_SERVER=OFF', '-DWITH_SHADOW=OFF', '-DWITH_SAMPLE=OFF',
    '-DWITH_OPENSSL=ON', '-DWITH_MEDIA_FOUNDATION=OFF', '-DWITH_DSP_FFMPEG=OFF',
    '-DWITH_SWSCALE=OFF', '-DWITH_CAIRO=OFF',
    '-DWITH_GFX_AV1=OFF', '-DWITH_GFX_AZURE=OFF', '-DWITH_AAD=OFF',
    '-DWITH_KRB5=OFF', '-DWITH_PKCS11=OFF', '-DWITH_WEBVIEW=OFF',
    '-DWITH_WINPR_TOOLS=OFF', '-DWITH_MANPAGES=OFF', '-DWITH_DOCUMENTATION=OFF',
    '-DWITH_FREERDP_DEPRECATED=ON', '-DWITH_FREERDP_DEPRECATED_COMMANDLINE=ON',
    '-DBUILD_TESTING=OFF'
)
if ($H264Decoder -eq 'FFmpeg') {
    if (!$SkipConfigure -and !(Test-Path (Join-Path $deps 'include/libavcodec/avcodec.h'))) {
        throw 'Install the x86 FFmpeg dependencies first; see MYRTILLE-MIGRATION.md.'
    }
    $options += '-DWITH_OPENH264=OFF', '-DWITH_FFMPEG=ON', '-DWITH_VIDEO_FFMPEG=ON'
} else {
    # Retained for decoder comparisons and synthetic encoder fixtures.
    $options += '-DWITH_OPENH264=ON', '-DWITH_FFMPEG=OFF', '-DWITH_VIDEO_FFMPEG=OFF'
}
if (!$SkipConfigure) {
    & $CMake @options
    if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
}
if (!$ConfigureOnly) {
    & $CMake --build $build --config Release --target wfreerdp myrtille-settings-test --parallel 4
    if ($LASTEXITCODE) { throw "Build failed: $LASTEXITCODE" }
    $stage = Join-Path $source 'artifacts/bin'
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Get-ChildItem $build -Recurse -File -Filter '*.dll' |
        Where-Object { $_.Directory.Name -eq 'Release' } |
        Copy-Item -Destination $stage -Force
    Get-ChildItem (Join-Path $deps 'bin') -File -Filter '*.dll' |
        Copy-Item -Destination $stage -Force
    Copy-Item (Join-Path $build 'client/Windows/cli/Release/wfreerdp.exe') $stage -Force
    Copy-Item (Join-Path $build 'client/Windows/Release/myrtille-settings-test.exe') $stage -Force
    & (Join-Path $stage 'myrtille-settings-test.exe')
    if ($LASTEXITCODE) { throw "Settings regression failed: $LASTEXITCODE" }
}
