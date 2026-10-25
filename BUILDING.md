# Building VIZR

This repository holds everything a release is built from: the emulator at the root, and the launcher, the profiles
and the installer under `vizr/`. It contains no console keys, firmware or game files, and the build downloads none.

## Requirements (Windows x64)
- Visual Studio 2026 with the C++ x64 tools (MSVC 14.50 or later), CMake 3.22 or later, Ninja, the Vulkan SDK.
- For the installer: Inno Setup 6.

## 1. Build eden.exe
In a Visual Studio x64 developer prompt, in this repository:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYUZU_TESTS=OFF -DYUZU_CMD=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DVIZR_OFFLINE=ON
    cmake --build build

The result is `build\bin\eden.exe`. Line 1 of its log names the commit it was built from. The third-party code is
fetched into `.cache\cpm` when CMake configures.

## 2. Put it in the installer's payload

    mkdir vizr\payload\build\eden\bin\user\config
    copy build\bin\eden.exe vizr\payload\build\eden\bin\
    copy vizr\config\qt-config.ini vizr\payload\build\eden\bin\user\config\

`vizr\payload\build\` is ignored by git.

## 3. Build the installer
The installer script reads the licences and the third-party note from `vizr\`, and `FORK-SOURCE.md`, which names
the commit you built:

    copy LICENSE.txt vizr\
    copy THIRD-PARTY-VIZR.md vizr\THIRD-PARTY.md
    xcopy /e /i LICENSES vizr\LICENSES
    echo Built from commit <commit> of this repository.> vizr\FORK-SOURCE.md
    ISCC.exe vizr\installer\setup.iss

ISCC is in the Inno Setup 6 folder. The installer is written to `vizr\VIZR-Setup-b<release number>.exe`.

## The release build
`.github/workflows/release.yml` runs these steps on GitHub's own runner for every release, checks the result (no
request hosts in eden.exe, line 1, an install and an uninstall), and attaches a build provenance attestation to every
file.
