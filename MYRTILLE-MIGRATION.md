# Myrtille / FreeRDP 3 migration

This is an isolated source tree. It does not replace the deployed gateway or the
`FreeRDP_sacmi` tree. All native DLLs must be built and distributed together:
the Myrtille callbacks extend the upstream client entry-point structure.

## Pinned sources

- FreeRDP tag `3.31.1`, commit `63b948ca5cb94307fd5444ee6e73927a41ccdab4`.
- vcpkg commit `386d7c478221b7ee0c97bfe6ea61dcf65121d564`.
- x86-windows dynamic dependencies: OpenSSL 3.6.4, FFmpeg 9.0.1#2,
  libwebp 1.6.0#3 and zlib 1.3.2#2.
- OpenH264 2.6.0#4 is retained for comparisons and synthetic encoder tests.

## Build

Requires Visual Studio 2026 C/C++ x86 tools, a Windows SDK and CMake 4.4.3.
Install dependencies locally (the first build requires network access):

```powershell
git clone https://github.com/microsoft/vcpkg.git .deps/vcpkg
git -C .deps/vcpkg checkout 386d7c478221b7ee0c97bfe6ea61dcf65121d564
& ./.deps/vcpkg/bootstrap-vcpkg.bat -disableMetrics
& ./.deps/vcpkg/vcpkg.exe install openssl:x86-windows zlib:x86-windows libwebp:x86-windows openh264:x86-windows "ffmpeg[core,avcodec,avformat,swresample,swscale]:x86-windows" --disable-metrics "--x-install-root=$PWD/.deps/installed"
./build-myrtille.ps1 -CMake /path/to/cmake.exe
```

The build uses x86 DLLs because the existing Myrtille.RDP host is x86. H.264
local decoding defaults to FFmpeg/libavcodec in software. OpenH264 is disabled
in that build: it is tried before FFmpeg upstream when both are enabled.
Use `-H264Decoder OpenH264` only for comparisons or encoder fixtures.
SDL, server/shadow, FFmpeg audio, USB redirection,
AV1, Azure/AAD, Kerberos and PKCS11 are not enabled in this Windows build.
Do not mix this output with the FreeRDP 2 DLLs.

Media Foundation is disabled: the unmodified 3.31.1 `h264_mf.c` fails compilation
(extra closing brace after `getSrcOffset`) and its YUV copy code also contains
incorrect offsets. It needs a separate upstream fix and decoder tests before
being enabled. Encoded video is forwarded to the browser, but FreeRDP still
decodes its native framebuffer. A bad native decode can therefore also affect
image updates sent by Myrtille alongside video.

Successful builds stage the native runtime in `artifacts/bin` and run the
settings regression. Then run:

```powershell
pwsh -File tests/smoke-pipes.ps1 -BinaryDirectory artifacts/bin
```

## Ported integration

- `StartRdpClient` keeps the existing .NET P/Invoke contract and pipe protocol.
- Custom settings use the FreeRDP 3 settings registry, including clone/free.
- AVC444 passthrough uses MAV2 packets carrying both luma/chroma components,
  their region masks, codec variant and presentation flag. The browser decodes
  them in order and reconstructs AVC444 rather than displaying auxiliary chroma
  as an ordinary RGB frame.
- Audio uses the upstream rdpsnd context accessor and does not require a local
  audio output device for Myrtille sessions.
- Clipboard redirects Unicode text through the browser; file clipboard access
  to the gateway is disabled for Myrtille sessions.
- Cursor/image and PDF print notifications retain the existing pipe messages.
- Hidden sessions suppress native presentation, keyboard hooks and credential
  popups; supplied credentials remain usable.
- On Windows, if OpenSSL cannot find its legacy provider, WinPR loads
  `legacy.dll` by absolute path beside the loaded libcrypto DLL. An explicitly
  configured `OPENSSL_MODULES` is respected. This keeps MD4 available for NTLM
  in the portable package without changing machine-wide environment variables.
- Legacy command-line aliases are enabled for the existing gateway. The removed
  `async-input` option is accepted with a warning and has no effect.

Historical core patches are not reapplied blindly: current upstream handles
UTF-8 CAL paths and printer handle cleanup. The old hostname-to-IP rewrite is
omitted so DNS names retain their normal authentication/certificate semantics.

## Validation and limits

Additional offline validation on 2026-09-21:

- Two private AVC444v2 captures (119 and 234 components) replay successfully
  through FFmpeg. OpenH264 produces rainbow artifacts on the same input.
- At component 30, all raw and reconstructed YUV samples match browser output.
  After all 234 components, the full final RGB image matches the browser exactly
  (zero differing RGB samples). Native unused alpha and browser opacity differ.
- The separately staged `artifacts/avc444-ffmpeg` runtime reproduces both replay
  outputs and passes the settings and x86 .NET/four-pipe regressions. It has not been deployed to the
  active A/B websites.
- Browser VideoFrame getter caching reduces measured reconstruction from about
  283 to 21-23 ms/presentation on this capture. This is not an end-to-end FPS claim.

See `tests/H264-INVESTIGATION.md` for reproduction details and remaining live
validation. Raw desktop captures and decoded images must remain private/local.

Validated locally on 2026-09-17:

- Release x86 build and native version output: FreeRDP 3.31.1 (`63b948c`).
- `myrtille-settings-test`: OpenSSL MD4 known vector, actual legacy gateway
  arguments, AVC444 settings and independent ownership after clone/free.
- `tests/smoke-pipes.ps1`: passed, including a Unicode clipboard input command.
- Existing gateway `tests/video-pipe-regression.ps1`: passed with 4 KB and
  256 KB read buffers and native messages up to 600000 bytes.

`tests/smoke-pipes.ps1` loads the native DLL via x86 .NET, connects all four pipes,
sends input settings, and closes the process without contacting an RDP server.

A live Windows RDP session is still needed to validate video quality and frame
rate, reconnect, clipboard in both directions, audio, PDF printing and server
certificate/authentication behavior. No performance gain is claimed from the
version upgrade alone. The browser video path and native framebuffer decoding
both need to remain correct.
Upstream marks the legacy Windows client as unmaintained; this port maintains
the existing integration instead of replacing it with SDL.

References: https://www.freerdp.com/2026/09/02/3_31_1-release and
https://github.com/FreeRDP/FreeRDP/wiki/FreeRDP3-migration-notes.
