# libntr

This is the separately maintained [community gameplay runtime](https://github.com/dr4lera/pokemon-platinum-together-open-sourced-client)
fork, based on cybervisi0n/libntr commit
`1555d911ac80c0190ac1c321f99f1323de8b3c09`. Original upstream notices remain intact.
Community changes were developed with OpenAI Codex assistance and are not submitted
upstream. The desktop SDK4 resource opener adds an explicitly selected private
asset root and a restricted patch overlay; no game assets are included.

The gameplay client's `tests/run_community_asset_overlay.py` checks the actual
resolver and FS opener, including patch preference, fallback, malformed paths
and symlink confinement. These checks do not establish full gameplay parity.
The gameplay client's `tests/run_community_wireless.py` also checks bounds and
established-peer identity binding in the legacy LAN parser, receive-buffer
lifetime through WM callback acknowledgment, packet cleanup and actual WM send
failure status. This LAN path does not authenticate community accounts. The
authenticated party/link transport is selected explicitly by the application
through the permanent `SIM_Net_Provider_t` table. The table routes semantic WM
operations; it does not expose or forward native ENet packet layouts. A failed
join now reports a failed ARM7 connect callback instead of claiming success.
Application-level integration and gameplay checks remain the client's scope.

Desktop QA can explicitly set `PLATINUM_COMMUNITY_CAPTURE` to a fresh `.ppm`
path. A one-shot capture reads only this application's OpenGL backbuffer after
180 frames (or `PLATINUM_COMMUNITY_CAPTURE_FRAME`, 1–36000). It never replaces an
existing file. `PLATINUM_COMMUNITY_SCENARIO` may select an absolute, private text
file containing at most 1024 ordered `decimal-frame hex-button-mask` records.
Frames are bounded to 36000, masks to the twelve emulated buttons, and the final
mask must release all buttons. This opt-in fixture drives only emulated inputs
inside this process; it never focuses a window or injects OS keyboard/mouse input.

![logo](images/libntr.png)
Drop-in replacement for NitroSDK that targets Nintendo DS/DSi, PC, and other platforms.

libntr makes it easier to port Nintendo DS/DSi games to PC and other platforms by handling the logic for common libraries used by nearly all Nintendo DS games.

See also: 
 * [libntrsystem](https://github.com/cybervisi0n/libntrsystem)
 * [libntrwifi](https://github.com/cybervisi0n/libntrwifi)
 * [libntrdwc](https://github.com/cybervisi0n/libntrdwc)

## Features
 * Fully compatible with NitroSDK
 * Translate DS 3D graphics APIs to OpenGL 4.2 with support for 3D upscaling
 * Simulate DS 2D graphics
 * Includes support for adding GUIs via Dear ImGui
 * Simulate local wireless multiplayer over LAN or the internet

## Available Build Targets
Select the build target with the 'build_target' meson option.
 * arm (Nintendo DS hardware)
 * linux (x86_64 Linux PC)
 * win64 (x86_64 Windows PC)
 * nx (Switch via libnx)

## DSi Support (BETA)
libntr supports building TWL applications. Set the meson options:
 * sdk_version_major=5
 * hardware_type=(twl_hyb) or (twl_ltd)
 * * twl_hyb: DSi enhanced application, supports running on original NDS and DSi
 * * twl_ltd: DSiWare, DSi only
