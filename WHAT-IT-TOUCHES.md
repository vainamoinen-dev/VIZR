# What VIZR touches on your PC

VIZR has three parts: the **installer**, the **launcher** (a PowerShell script started by the desktop shortcut), and
**Eden**, the emulator, built from our fork with the VR layer inside it. This page lists, from the source, every file,
folder, setting, registry key and outside program each part uses, and what each part can do on the network.

Citations look like `path:line`, in this repository. Paths that start with `vizr/` are the installer and the launcher;
`.github/` is the release build and its checks; paths that start with `src/`, `CMakeLists.txt`, `CMakeModules/` or
`externals/` are the Eden fork.
"`{app}`" means the install folder (by default `%LOCALAPPDATA%\VIZR`). "`bin`" means `{app}\build\<build>\bin`, the
folder that holds `eden.exe`.

## 1. Summary

| Part | Files and folders | Registry | Network |
|---|---|---|---|
| Installer | Writes only inside `{app}`, plus shortcuts on your desktop and Start menu and a setup log in `%TEMP%`. Reads the keys, game and firmware files you pick. | Reads the OpenXR runtime key. Inno Setup writes its own per-user uninstall entry. | None |
| Launcher | Reads and writes inside `{app}` only: Eden's settings file, a staged copy of the game profile, the mod patch folder, session logs, a backup of your game saves at every start. | Reads the OpenXR runtime key. | None |
| Eden (this build) | Everything in the portable `bin\user` folder next to `eden.exe`, plus the game file you chose. | Reads only (OS version, OpenXR, and system libraries' own lookups). No writes. | Eden's own requests are not built. Game sockets and name lookups are refused while airplane mode is on (the default). |
| VR runtime | Not part of VIZR. It loads inside `eden.exe`. | Its own | Its own (for example, a streamer app on this PC or your network) |

## 2. The installer

The installer is an Inno Setup script, `vizr/installer/setup.iss`.

- **Per user, no admin.** `PrivilegesRequired=lowest` (`vizr/installer/setup.iss:52`). The default folder is
  `{localappdata}\VIZR` (`vizr/installer/setup.iss:50`).
- **Files it copies.** The payload into `{app}` (`vizr/installer/setup.iss:66-67`). The payload is `eden.exe`
  alone (no debug symbols, no room server: `.github/workflows/release.yml:180`), a cleaned Eden settings file with no
  paths and an empty web token (`vizr/config/qt-config.ini`), the game profiles and patches (`vizr/payload/profiles`,
  `vizr/payload/patches`), the launcher scripts (`vizr/payload/tools/vizr`), and the `.cmd` the shortcut runs
  (`vizr/payload/VIZR.cmd`).
- **Files it reads.** The first 64 KB of your `prod.keys`, to check it (`vizr/installer/setup.iss:158-164`). The
  first 1 MB of your game file, to check its type and title (`vizr/installer/setup.iss:188-198`). The size of the
  game file (`vizr/installer/setup.iss:292-302`).
- **Files it writes after copying** (`vizr/installer/setup.iss:360-434`):

| What | Where | Source |
|---|---|---|
| Your keys (and `title.keys` if it sits beside them) | `bin\user\keys\` | `vizr/installer/setup.iss:370-373` |
| Firmware, only if you pick it | `bin\user\nand\system\Contents\registered` | `vizr/installer/setup.iss:377-396` |
| The game, only if you tick "copy" (unticked by default) | `{app}\game\` | `vizr/installer/setup.iss:258-261`, `:400-415` |
| `GAME.txt` (the game's path and title id) | `{app}\game\` | `vizr/installer/setup.iss:419` |
| `INSTALL-CHECK.txt` (what was checked) | `{app}` | `vizr/installer/setup.iss:430-433` |
| Setup log | `%TEMP%\Setup Log <date> #<n>.txt` | `vizr/installer/setup.iss:62-63` |

- **Shortcuts.** One on your desktop and one in your Start menu, both pointing at `{app}\VIZR.cmd`, under one task
  that is ticked by default (`vizr/installer/setup.iss:106-112`).
- **Programs it starts.** `powershell.exe` with `Expand-Archive`, only to unpack a firmware zip you picked
  (`vizr/installer/setup.iss:380-381`).
- **Registry.** It reads `HKLM\SOFTWARE\Khronos\OpenXR\1`, value `ActiveRuntime` (the 64-bit view), to report whether
  a VR runtime is installed (`vizr/installer/setup.iss:200-211`). The script itself writes no registry values.
  It does not turn off Inno's uninstaller (there is no `Uninstallable=` line in `vizr/installer/setup.iss:41-64`),
  so Inno Setup writes its standard uninstall entry for a per-user install under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B2E1F0A-5D4C-4E7B-9A31-3D5E2C7A9B10}_is1`, and adds its
  `unins000.exe` and `unins000.dat` to `{app}`. This is Inno Setup's documented behaviour: the uninstaller is included
  by default (`Uninstallable`, default `yes`: https://jrsoftware.org/ishelp/topic_setup_uninstallable.htm), Setup
  creates the Add/Remove Programs entry by default (`CreateUninstallRegKey`, default `yes`:
  https://jrsoftware.org/ishelp/topic_setup_createuninstallregkey.htm), in non-administrative install mode "the HKA,
  uninstall info, and font install root keys will be HKEY_CURRENT_USER"
  (https://jrsoftware.org/ishelp/topic_admininstallmode.htm), and the key's name is the `AppId` with "_is1" added
  (https://jrsoftware.org/ishelp/topic_setup_appid.htm; our `AppId` is `vizr/installer/setup.iss` `AppId=`).
- **Network.** None. The script has no download or web call (searched for web requests, `curl`, BITS and `http`).

## 3. The launcher

The shortcut runs `VIZR.cmd`, which sets `VIZR_ROOT` to the install folder and runs `tools\vizr\launch.ps1` in
Windows PowerShell (`vizr/payload/VIZR.cmd:5-6`). The launcher loads `render-scale.ps1`
(`vizr/payload/tools/vizr/launch.ps1:219`) and the game's staging script `vizr/payload/tools/vizr/games/<title>.ps1`
(`vizr/payload/tools/vizr/launch.ps1:63-64`), which loads `vizr/payload/tools/vizr/stage/stage-render.ps1` and `stage-modec.ps1`
(`vizr/payload/tools/vizr/games/010012101468C000.ps1:7-8`). The stored-save and menu-press helpers are development tools and are
not in the public install (they are not in `vizr/payload/tools/vizr`); `-Save` stops with a message there
(`vizr/payload/tools/vizr/launch.ps1:196`).

**What it reads**

- `{app}\game\GAME.txt` for the game and title (`vizr/payload/tools/vizr/launch.ps1:46-59`), and checks the game file exists (`:100`).
- The installed profile `{app}\profiles\<title>\` (`vizr/payload/tools/vizr/launch.ps1:62`) and your `user.toml` there for the
  Compatibility settings (`vizr/payload/tools/vizr/launch.ps1:150`, `:207-214`).
- Earlier session logs in `{app}\logs`, for the last per-eye size and frame-rate history
  (`vizr/payload/tools/vizr/render-scale.ps1:47-93`).
- The registry key `HKLM\SOFTWARE\Khronos\OpenXR\1` `ActiveRuntime` (`vizr/payload/tools/vizr/launch.ps1:88`), and whether the
  `VirtualDesktop.Streamer` process is running (`vizr/payload/tools/vizr/launch.ps1:89`). Nothing is written to the registry.

**What it writes** (all inside `{app}`)

| What | Where | Source |
|---|---|---|
| A session folder per launch: `launch.txt`, copies of the profile used | `{app}\logs\<date>-session<N>\` | `vizr/payload/tools/vizr/launch.ps1:72-78`, `:139`, `:266-289` |
| A copy of your game saves at every start (`bin\user\nand\user\save`, only read); the newest 10 copies are kept and older ones in that folder are deleted. An uninstall leaves them | `{app}\save-backups\<date>-session<N>\` | `vizr/payload/tools/vizr/launch.ps1:171-191` |
| A full copy of `{app}\profiles`, which the game's launch settings are applied to (the installed profile is only read) | `...\session<N>\profiles-run\` | `vizr/payload/tools/vizr/launch.ps1:141-147`, `vizr/payload/tools/vizr/games/010012101468C000.ps1:12-32` |
| The mod patches Eden loads: the folder's `.pchtxt` files are deleted, then the enabled ones are written | `bin\user\load\010012101468C000\VIZR-C\exefs\` | `vizr/payload/tools/vizr/stage/stage-modec.ps1:38-53` |
| Eden's settings file: folder paths set to this install, the game folder added, resolution, shader mode, GPU accuracy | `bin\user\config\qt-config.ini` | `vizr/payload/tools/vizr/launch.ps1:155-169`, `:225-242`, `vizr/payload/tools/vizr/render-scale.ps1:35-38` |
| The last session's Eden log, if its window closed early | `{app}\logs\...\eden_log.txt` | `vizr/payload/tools/vizr/launch.ps1:122-136` |
| GPU and CPU samples once a second, then Eden's log and closing lines | `...\session<N>\gpu.csv`, `cpu.csv`, `eden_log.txt`, `launch.txt` | `vizr/payload/tools/vizr/session-keeper.ps1:19-21`, `:44-74`, `:95` |

**Environment variables** (for this launch only, inherited by Eden; nothing is saved to your user settings)

- Set: `VIZR_USER_TOML`, `VIZR_PROFILES` (`vizr/payload/tools/vizr/launch.ps1:151-152`), `VIZR_RENDER_SCALE`
  (`vizr/payload/tools/vizr/launch.ps1:248`), `VIZR_RENDER_SCALE_MAX` (`vizr/payload/tools/vizr/launch.ps1:256`), `VIZR_SESSION_WRITES` when
  asked (`vizr/payload/tools/vizr/launch.ps1:116`). `VIZR.cmd` sets `VIZR_ROOT` (`vizr/payload/VIZR.cmd:5`).
- Cleared, so a test setting from a shell can never redirect the runtime: `XR_RUNTIME_JSON`, `VIZR_RIG_OUT`,
  `VIZR_RIG_WRITES`, `VIZR_RIG_CAM_BACK`, `VIZR_LAG_PROBE`, `VIZR_XR_RUNTIME_ALLOW` (`vizr/payload/tools/vizr/launch.ps1:112-115`).

**Programs it starts**

- `eden.exe -g "<your game>"` (`vizr/payload/tools/vizr/launch.ps1:300`).
- A hidden `powershell.exe` running `session-keeper.ps1`, tied to Eden's process (`vizr/payload/tools/vizr/launch.ps1:303-304`).
  It runs `nvidia-smi` once a second if that program is installed (`vizr/payload/tools/vizr/session-keeper.ps1:19-21`, `:47`),
  reads CPU load through WMI (`vizr/payload/tools/vizr/session-keeper.ps1:51`), and reads Eden's thread names with the Windows
  `OpenThread` and `GetThreadDescription` calls (`vizr/payload/tools/vizr/session-keeper.ps1:23-43`). Windows PowerShell compiles
  that small helper at start (`Add-Type`). Watched on a test PC: the C# compiler writes six files with one random
  name in `%TEMP%` (`.0.cs`, `.cmdline`, `.dll`, `.out`, `.err`, `.tmp`) plus a `CSC*.TMP` and a `RES*.tmp`, and all
  of them are deleted within about 60 ms. Every PowerShell start also writes and deletes two
  `__PSScriptPolicyTest_*` files there. Nothing is left behind.

**Network.** None. No web request, download, `curl`, BITS or `http` appears in `launch.ps1`, `render-scale.ps1`,
`session-keeper.ps1`, `games\*.ps1` or `stage\*.ps1`.

## 4. Eden (as built: build 52)

### Files

Eden is portable. On Windows it uses a folder named `user` next to `eden.exe` when that folder exists, and only
otherwise `%APPDATA%\eden` (`src/common/fs/path_util.cpp:115-119`, `src/common/fs/fs_paths.h:11-12`). The installer
ships `bin\user\config\qt-config.ini` (`vizr/config/qt-config.ini`, placed by `.github/workflows/release.yml:181`), so the `user` folder always exists and
Eden does not use `%APPDATA%`. Eden offers to import another emulator's data only when its config folder is missing
(`src/yuzu/user_data_migration.cpp:33`), which it is not here.

Inside `bin\user` (`src/common/fs/path_util.cpp:155-176`): `config` (settings), `keys`, `nand` (console data and your
saves in `nand\user\save`), `sdmc`, `load` (mods), `cache` (shader cache), `log` (`eden_log.txt`), `play_time`,
`screenshots`, `dump`, `tas`, `amiibo`, `icons`, `crash_dumps`. Eden also reads the game file you chose.

The VR layer reads the profile folder named by `VIZR_PROFILES` (`src/core/vizr/hook_runtime.cpp:6830`) and saves
your in-headset settings to `user.toml` (the path in `VIZR_USER_TOML`, else the profile folder;
`src/core/vizr/user_settings.cpp:91-96`) and `runtime.toml` beside it (`src/core/vizr/user_settings.cpp:60`). Each
save writes a temporary `.tmp-<n>` file and then renames it (`src/core/vizr/user_settings.cpp:268`, `:301`); a file
that cannot be read is kept as `.bak-<time>` first (`src/core/vizr/user_settings.cpp:199`).

Only when you use these menu items does Eden write elsewhere: `%TEMP%\eden\` while installing firmware or a mod zip
(`src/qt_common/util/content.cpp:184`, `src/qt_common/util/mod.cpp:88`, `:123`), and a game shortcut on your desktop
or Start menu (`src/qt_common/util/game.cpp:504-506`).

### Settings

Eden keeps its settings in INI files, not the registry (`src/qt_common/gui_settings.cpp:19-23`). Settings that matter
for the network: **airplane mode, on by default** (`src/common/settings.h:937`), set by you in Eden's Network settings
(`src/yuzu/configuration/configure_network.cpp:26`); the debugger (GDB stub), **off by default**, port 6543 on this PC
only (`src/common/settings.h:872-873`); reporting services, off by default (`src/common/settings.h:889-890`); the web
service address, blank in this build (`src/common/settings.h:940-945`).

### Registry

All reads, no writes. There is no `RegSetValue`, `RegCreateKey` or `RegDelete` call in the fork's `src` tree, and the
only registry-format settings object is a read (`src/qt_common/qt_common.cpp:176-185`).

| Reader | Key | Source |
|---|---|---|
| Eden, for the log's OS name | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` (`ReleaseId`, `CurrentBuild`, `DisplayVersion`, `UBR`) | `src/qt_common/qt_common.cpp:176-185` |
| VIZR, to log which VR runtime will load (skipped when `XR_RUNTIME_JSON` is set) | `HKLM\SOFTWARE\Khronos\OpenXR\1` `ActiveRuntime` | `src/video_core/renderer_vulkan/openxr/xr_runtime.cpp:48-62` |
| The OpenXR loader (linked into `eden.exe`) | `HKLM\SOFTWARE\Khronos\OpenXR\1` `ActiveRuntime` and the `ApiLayers` keys | `externals/CMakeLists.txt:268-270`, `src/video_core/CMakeLists.txt:363`; the Khronos loader specification |
| The Windows Vulkan loader (`vulkan-1.dll`) | `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, `HKLM`/`HKCU\SOFTWARE\Khronos\Vulkan\ExplicitLayers` and `ImplicitLayers`, the display adapter's driver key | `Vulkan-Loader/loader/loader_windows.c` (opens keys for query only) |
| Qt's Windows plugin | `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` `AppsUseLightTheme`; `HKCU\Software\Microsoft\Windows\DWM` `AccentColor` | qtbase 6.11.1 `qtbase/src/plugins/platforms/windows/qwindowstheme.cpp` |
| SDL3 | the same `Personalize` value; `HKLM\System\CurrentControlSet\Control\MediaCategories\{GUID}` `Name` | `SDL/src/video/windows/SDL_windowsvideo.c`, `SDL/src/core/windows/SDL_windows.c` |

Qt's other Windows plugin files and SDL's other input backends were not checked for registry use.

### Network

This build is configured with `VIZR_OFFLINE=ON`, the room server and its standalone program off, the web service off
and the update checker off (`.github/workflows/release.yml:107`; option at `CMakeLists.txt:278`, define at
`CMakeLists.txt:327-328`). With `VIZR_OFFLINE`, the release and update host names are blanked at configure time
(`CMakeModules/GenerateSCMRev.cmake:72-79`).

| Network path | State in this build | Source |
|---|---|---|
| Web service (telemetry, login, room listing) | **Not built** | `src/CMakeLists.txt:289-291`; lobby uses an empty backend: `src/network/announce_multiplayer_session.cpp:23-29` |
| Telemetry | **Not built** (it is part of the web service) | `CMakeLists.txt:259`, `src/CMakeLists.txt:289-291` |
| Update checker | **Not built** | `src/frontend_common/CMakeLists.txt:19-24`; `src/yuzu/main_window.cpp:122`, `:454` |
| Update dialog (downloads an update) | Compiled, **never reached** (its only use is inside the update-checker block), and its download returns at once | `src/yuzu/CMakeLists.txt:237`; `src/yuzu/main_window.cpp:4077-4086`; `src/yuzu/updater/update_dialog.cpp:78-81` |
| Eden's shared web request function | **Off**: returns without a request | `src/common/net/net.cpp:207-212` |
| Discord presence | **Not built** | `CMakeLists.txt:262`; `src/qt_common/CMakeLists.txt:51-62` |
| Built-in news (BCAT): release list and images | **Off**: no fetch, no image download, no link host | `src/core/hle/service/bcat/news/builtin_news.cpp:467-473`, `:83-86`, `:430-434` |
| Amiibo data lookup | **Not built** (web-service only, and not called) | `src/yuzu/applets/qt_amiibo_settings.cpp:92`, `:99-102` |
| Room server and `eden-room.exe` | **Not built**, not shipped | `src/CMakeLists.txt:254-259`; `src/yuzu/CMakeLists.txt:443-444`; only `eden.exe` is shipped: `.github/workflows/release.yml:180` |
| Room client (ENet UDP) | Compiled; the **Multiplayer menu is hidden**, so there is no way to create or join a room | `src/CMakeLists.txt:252`; `src/network/CMakeLists.txt:24`; `src/yuzu/main_window.cpp:1017-1022` |
| LDN (local wireless play) | Runs only over a joined room, so **no traffic** | `src/core/hle/service/ldn/lan_discovery.cpp:416`, `:474` |
| Game sockets, TCP and UDP (BSD service) | **Refused by airplane mode**, before any host socket is made | `src/core/hle/service/sockets/bsd.cpp:544-551` |
| Game name lookups (sfdnsres, which would use the host's `getaddrinfo`) | **Refused by airplane mode**: no host name is resolved | `src/core/hle/service/sockets/sfdnsres.cpp:100-109`, `:212`, `:328`; `src/core/internal_network/network.cpp:741` |
| Game SSL | Uses the game sockets above, so **refused by airplane mode** | `src/core/hle/service/sockets/bsd.cpp:544-551` |
| Network status (nifm) | Reports "not connected" in airplane mode; a game **cannot turn airplane mode off** | `src/core/hle/service/nifm/nifm.cpp:918-936`, `:989`, `:1001` |
| Debugger (GDB stub) | Compiled, **off by default**; when you turn it on it listens on TCP port 6543 on **this PC only** (127.0.0.1) | `src/common/settings.h:872-873`; `src/core/debugger/debugger.cpp:138-157` |
| Motion controller over UDP (the CemuHook protocol, a phone or a pad server on your PC) | **Opens no socket unless you turn it on** (Eden > Settings > Controls, off by default; turning it on takes effect at Eden's next start, or after OK in the Configure Motion / Touch window); upstream Eden opens one at every launch, bound to every interface and polling 127.0.0.1:26760 | `src/input_common/drivers/udp_client.cpp:156-166`, `:36`; `src/common/settings.h:807-809`; `src/yuzu/configuration/configure_motion_touch.cpp:303` |
| Help menu links | Open in **your web browser** when clicked; Eden makes no request | `src/yuzu/main_window.cpp:3218-3231` |
| Building from source | Downloads libraries at configure time; nothing at run time | `externals/CMakeLists.txt:270` (`AddJsonPackage`) |

**If you turn airplane mode off** (Eden > Settings > Network): game sockets, TCP and UDP, become real sockets on your
PC (`src/core/hle/service/sockets/bsd.cpp:558-571`); game name lookups go to your PC's DNS, except a built-in list of
blocked domains (`src/core/hle/service/sockets/sfdnsres.cpp:106-108`, `:217`, `:341`); and nifm reports a connection
when your PC has an IPv4 address (`src/core/hle/service/nifm/nifm.cpp:989`, `:1001`). Eden's own requests above stay
off either way, because they are compiled out, not switched by this setting.

The game's own code imports no network functions (its import table), the game is not permitted to use the news
service (its service access list), and Eden's logs show no network service use during play.

## 5. The VR runtime

Eden uses whatever OpenXR runtime Windows has set as active. The OpenXR loader inside `eden.exe` finds it through
`HKLM\SOFTWARE\Khronos\OpenXR\1` `ActiveRuntime`, or through `XR_RUNTIME_JSON` when that variable is set
(`src/video_core/renderer_vulkan/openxr/xr_runtime.cpp:48-62`). The launcher never sets `XR_RUNTIME_JSON` and clears
it if a shell had it (`vizr/payload/tools/vizr/launch.ps1:112-115`). Test variables (`VIZR_NO_VR`, `VIZR_RIG_OUT`) make Eden refuse
any runtime outside a test folder (`src/video_core/renderer_vulkan/openxr/xr_runtime.cpp:195`).

The runtime is a DLL from your VR software. It loads into `eden.exe` and runs there. It may talk to its own streamer
or service app on this PC or on your local network to reach the headset. That traffic is the runtime's behaviour, not
Eden's or VIZR's, and it is not covered by airplane mode.

## 6. What uninstall removes and what it keeps

**Removed:** every file the installer copied (Inno's standard behaviour), the shortcuts, Inno's uninstall entry, and
these extra files and folders (`vizr/installer/setup.iss:74-104`):

- `bin\user\keys` (your copied keys), `bin\user\log`, `{app}\INSTALL-CHECK.txt`;
- `{app}\game` (`GAME.txt`, and the game and update copies if you made them; your original game file is never touched);
- `bin\user\cache` (shader cache), `bin\user\nand\system` (console data and firmware), `bin\user\sdmc`,
  `bin\user\play_time`, `bin\user\config` (Eden's settings);
- `{app}\logs` (Eden's logs, the per-launch profile copies, `gpu.csv`, `cpu.csv`), `bin\user\load` (the staged mod
  patches), and `runtime.toml` and any `.bak-*` copies in `{app}\profiles\010012101468C000\`.

**Kept:**

- **Your game saves** in `bin\user\nand\user\save` (on purpose: `vizr/installer/setup.iss:75-76`, `:82`).
- Your VR settings, `user.toml` in `{app}\profiles\010012101468C000\`.
- Anything else Eden made in `bin\user` that is not listed above (for example `screenshots`, `dump`, `tas`).
- The setup log in `%TEMP%`.

Because these files remain, the `{app}` folder stays after uninstall. Delete it by hand to remove everything,
including your saves.

## 7. How this is checked

- **Static check:** `.github/scripts/net-static-check.ps1` reads the built `eden.exe` and fails if it contains any of
  Eden's request hosts or paths (news images and links, the release list path, the update hosts, the amiibo data
  hosts) in 8-bit or UTF-16 text, or if the build folder holds a room server (`.github/scripts/net-static-check.ps1:3-6`,
  `:18-40`).
- **Runtime gate:** a test run watches every `eden.exe`, `eden-room.exe` and launcher PowerShell
  process during a real run. It records TCP and UDP traffic and DNS queries by process (Windows event tracing, when
  installed), and also polls open connections every 100 ms and compares the DNS cache before and after. The DNS cache comparison is **not reliable**: on the control build (Eden's own
  news fetch left on) it showed no new name, most likely because the name was already cached from an earlier run; the
  100 ms connection polling is what caught that build's 30 HTTPS connections. Without event tracing, a pass means
  "none observed", not proof; the proof is the code above and the static check.
- **Result for build 51 (2026-10-24):** static check PASS (none of the request hosts or paths in the
  program, no room server, `VIZR_OFFLINE=ON`). Two 5-minute runs, one idle and one that opens the built-in news on
  purpose: PASS, 0 connections seen in about 3,400 polls each, event tracing off, so "none observed". The news run
  logged "built-in news is off in this build" and "no UDP input socket". The control build (network code left on)
  failed as it should: 30 HTTPS connections to a public address.
