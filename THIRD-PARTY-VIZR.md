# Third-party code in VIZR

VIZR is a modified fork of the Eden emulator (https://git.eden-emu.dev/eden-emu/eden) from upstream commit aaa0bcb9bf
(GPL-3.0-or-later; see LICENSE.txt and the LICENSES folder for every license that
applies to this source tree). The code it is built from:

## Code used
| Component | Used for | License | Copyright |
|---|---|---|---|
| Eden (this fork's base) | the emulator: everything outside the VR layer | GPL-3.0-or-later | Eden Emulator Project; yuzu Emulator Project |
| Dear ImGui | the in-headset settings menu (src/video_core/renderer_vulkan/openxr/xr_menu.cpp) | MIT | Omar Cornut and contributors |
| OpenXR SDK (headers and loader) | talking to the VR runtime (src/video_core/renderer_vulkan/openxr) | Apache-2.0 | The Khronos Group Inc. |
| toml++ | reading game profiles and the player's settings (src/core/vizr) | MIT | Mark Gillard and contributors |

## Prebuilt libraries
The Windows build links these as prebuilt static libraries, which CMake fetches when it configures (each release's
third-party-sources zip holds the files that build used). Each is built from the upstream source named here.

| Library | Version | Upstream source | Prebuilt by |
|---|---|---|---|
| Qt (qtbase, qtcharts, qttools) | 6.11.1 | https://download.qt.io/official_releases/qt/6.11/6.11.1/submodules/ (qtbase-, qtcharts- and qttools-everywhere-src-6.11.1.tar.xz) | https://github.com/Eden-CI/Qt/releases/tag/6.11.1 |
| FFmpeg | 9.0.2 (tag n9.0.2, commit 946fcce07b6dcd0331c8cc609192aeff5e1924f8) | https://github.com/FFmpeg/FFmpeg/tree/n9.0.2 | https://github.com/crueter-ci/FFmpeg/releases/tag/9.0.2-1790726865-946fcce07b |
| OpenSSL | commit b64f68a94e61fa2363c598c75444482b48056697 (labelled 4.0.1 by its build) | https://github.com/openssl/openssl/tree/b64f68a94e61fa2363c598c75444482b48056697 | https://github.com/crueter-ci/OpenSSL/releases/tag/4.0.1-1788234794-b64f68a94e |
| SDL3 | 3.4.14 (tag release-3.4.14, commit 147a8ee32dbf9ac02f3794964490687b6bbda1bc) | https://github.com/libsdl-org/SDL/tree/release-3.4.14 | https://github.com/crueter-ci/SDL3/releases/tag/3.4.14-1788231389-147a8ee32d |
| sirit | v1.0.7 | https://github.com/eden-emulator/sirit/tree/v1.0.7 | https://github.com/eden-emulator/sirit/releases/tag/v1.0.7 |

The installer is built with Inno Setup (Inno Setup License; Jordan Russell and Martijn Laan).
