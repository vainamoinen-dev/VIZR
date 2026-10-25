; SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
; SPDX-License-Identifier: GPL-3.0-or-later
;
; The public installer. One question per page: your prod.keys, your
; firmware (optional), your game, the install folder. Installs under the user's own files (no
; admin), puts a shortcut on the desktop, and ends with a plain checklist of what was verified. It ships no game, keys
; or firmware: the user supplies their own. export-public.ps1 fills in 52, eden, 623d724a2f, the project name
; VIZR, its repository https://github.com/vainamoinen-dev/VIZR, the released games Metroid Prime Remastered / 010012101468c000 (title ids, lower case, ';'-separated) and the icon VIZR.ico / VIZR.ico.
;
; Unattended (tests): <name>-Setup.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="C:\x" /KEYS="C:\prod.keys" /GAME="C:\game.nsp"
;   [/FIRMWARE="C:\fw.zip" or a folder] [/COPYGAME=1]
; No game update page: the profiles are written for the base game's program, which an update replaces (the launcher
; refuses an update NSP in game\).

#define AppName "VIZR"
; Build, BinDir and ForkHash can come from the command line (ISCC /DBuild=44 /DBinDir=eden /DForkHash=...): a
; source checkout builds its own release (BUILDING.md); the export's values are the defaults.
#ifndef Build
  #define Build "52"
#endif
#ifndef BinDir
  #define BinDir "eden"
#endif
#ifndef ForkHash
  #define ForkHash "623d724a2f"
#endif
; eden.exe links Microsoft's C++ runtime (vcruntime140, msvcp140): the installed runtime must be at least the version
; of the MSVC tools that built it (Microsoft: "the same or later"). The release workflow passes the runner's minor
; (/DVcMinor=51 for MSVC 14.51); 51 otherwise. The finish page checks it (read only) and names the download when older.
#ifndef VcMinor
  #define VcMinor "51"
#endif
#define VcUrl "https://aka.ms/vc14/vc_redist.x64.exe"
#define GameNames "Metroid Prime Remastered"
#define AppUrl "https://github.com/vainamoinen-dev/VIZR"
#define TitleIds "010012101468c000"
; The shortcut icon inside {app}; SetupIcon is the payload .ico for the setup exe, empty for Inno's own.
#define IconFile "VIZR.ico"
#define SetupIcon "VIZR.ico"

[Setup]
AppId={{6B2E1F0A-5D4C-4E7B-9A31-3D5E2C7A9B10}
AppName={#AppName}
AppVersion=build {#Build}
AppVerName={#AppName} (build {#Build})
AppPublisher={#AppName}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
; Clean-machine test: not Documents (often cloud-synced); per-user program data belongs in LocalAppData.
DefaultDirName={localappdata}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputBaseFilename={#AppName}-Setup-b{#Build}
OutputDir=..\
#if SetupIcon != ""
SetupIconFile=..\payload\{#SetupIcon}
#endif
UninstallDisplayIcon={app}\{#IconFile}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The install log (a support request attaches it): %TEMP%\Setup Log <date> #<n>.txt.
SetupLogging=yes
LicenseFile=..\LICENSE.txt

[Files]
Source: "..\payload\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion
; The licences of the code in eden.exe and this release, and where its source is: installed beside it.
Source: "..\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "..\THIRD-PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\FORK-SOURCE.md"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
; What setup wrote after copying its files: the user's keys, the game pointer, the check list, logs, the copied game
; (game\*). Saves (bin\user\nand\user\save) are kept, like any game's saves.
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\keys"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\log"
Type: files; Name: "{app}\INSTALL-CHECK.txt"
Type: filesandordirs; Name: "{app}\game"
; What Eden made while running (p2-verify3 uninstall left these): the shader cache (game-derived), console system
; data, play time, the SD log, per-game config. The user's game saves (nand\user\save) stay.
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\cache"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\nand\system"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\sdmc"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\play_time"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\config"
; The rest of Eden's user folder (made at its first start, or during play): installed content, the shader folder, the
; folders for amiibo data, crash dumps, icons, frame generation and post-processing.
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\nand\user\Contents"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\shader"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\amiibo"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\crash_dumps"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\icons"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\lossless"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\post_presets"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\post_shaders"
; What the launcher and the VR layer wrote: the session logs (Eden's log, the staged profile copy, gpu.csv, cpu.csv),
; the staged patch folder Eden loads, the remembered in-game state and backups of settings files that did not parse.
; The player's own settings (user.toml), saves, screenshots, dumps and TAS files stay.
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\build\{#BinDir}\bin\user\load"
Type: files; Name: "{app}\profiles\010012101468C000\runtime.toml"
Type: files; Name: "{app}\profiles\010012101468C000\*.bak-*"

[Tasks]
; Ticked by default (a desktop shortcut, no digging through folders); unattended tests pass /TASKS="".
Name: "shortcuts"; Description: "Put a {#AppName} shortcut on the desktop and in the Start menu"

[Icons]
Name: "{userdesktop}\{#AppName}"; Filename: "{app}\{#AppName}.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\{#IconFile}"; Tasks: shortcuts
Name: "{userprograms}\{#AppName}"; Filename: "{app}\{#AppName}.cmd"; WorkingDir: "{app}"; IconFilename: "{app}\{#IconFile}"; Tasks: shortcuts

[Code]
const
  TitleIds = '{#TitleIds}';
var
  KeysPage, GamePage, FirmwareZipPage: TInputFileWizardPage;
  FirmwareDirPage: TInputQueryWizardPage;
  CopyPage: TInputOptionWizardPage;
  KeysOk, FirmwareOk, GameOk, RuntimeOk, CopyFailed, VcOk: Boolean;
  FirmwareNote, GameNote, RuntimeNote, VcNote: String;

function Param(const Name: String): String;
begin
  Result := ExpandConstant('{param:' + Name + '|}');
end;

{ The first Count bytes of a file as text (binary bytes kept; NULs become spaces), lower case. }
function HeadText(const FileName: String; Count: Integer): String;
var
  Stream: TFileStream;
  Buffer: AnsiString;
  N, I: Integer;
begin
  Result := '';
  try
    Stream := TFileStream.Create(FileName, fmOpenRead or fmShareDenyNone);
    try
      N := Count;
      { Inno's stream size is 32-bit: a game over 2 GB reads as negative, so only clamp to a positive size. }
      if (Stream.Size > 0) and (Stream.Size < N) then N := Stream.Size;
      SetLength(Buffer, N);
      if N > 0 then Stream.ReadBuffer(Buffer, N);
      for I := 1 to N do
        if Buffer[I] = #0 then Buffer[I] := ' ';
      Result := Lowercase(String(Buffer));
    finally
      Stream.Free;
    end;
  except
    Log('{#AppName}: could not read ' + FileName + ': ' + GetExceptionMessage);
    Result := '';
  end;
end;

{ prod.keys: a text file with header_key and at least one key area key. }
function KeysLookRight(const FileName: String): Boolean;
var
  Text: String;
begin
  Text := HeadText(FileName, 65536);
  Result := (Pos('header_key', Text) > 0) and (Pos('key_area_key_application', Text) > 0);
end;

{ The first released title (TitleIds, ';'-separated) the text names, or ''. }
function ReleasedTitleIn(const Text: String): String;
var
  Ids, Id: String;
  P: Integer;
begin
  Result := '';
  Ids := TitleIds + ';';
  while (Ids <> '') and (Result = '') do begin
    P := Pos(';', Ids);
    Id := Copy(Ids, 1, P - 1);
    Ids := Copy(Ids, P + 1, Length(Ids));
    if (Id <> '') and (Pos(Id, Text) > 0) then Result := Id;
  end;
end;

function NamesReleasedTitle(const Text: String): Boolean;
begin
  Result := ReleasedTitleIn(Text) <> '';
end;

{ The game: an NSP (PFS0) or XCI (HEAD at 0x100) whose header names a released title's ticket or content. }
function GameKind(const FileName: String): String;
var
  Text: String;
begin
  Result := '';
  Text := HeadText(FileName, 1048576);
  if Copy(Text, 1, 4) = 'pfs0' then Result := 'NSP'
  else if Copy(Text, 257, 4) = 'head' then Result := 'XCI';
  if (Result <> '') and (Result = 'NSP') and not NamesReleasedTitle(Text) then
    Result := Result + ' (no supported title?)';
end;

function OpenXrRuntime: String;
var
  Value: String;
begin
  Result := '';
  { The 64-bit registry view: Eden is 64-bit (a 32-bit setup otherwise reads the runtime's 32-bit entry). }
  if IsWin64 then begin
    if RegQueryStringValue(HKLM64, 'SOFTWARE\Khronos\OpenXR\1', 'ActiveRuntime', Value) then
      if FileExists(Value) then Result := Value;
  end else if RegQueryStringValue(HKLM, 'SOFTWARE\Khronos\OpenXR\1', 'ActiveRuntime', Value) then
    if FileExists(Value) then Result := Value;
end;

{ The Visual C++ v14 runtime (x64) as Microsoft's redistributable records it: HKLM\SOFTWARE\Microsoft\VisualStudio\
  14.0\VC\Runtimes\X64 (Installed, Major, Minor, Bld; the 64-bit view). Read only. True when it is 14.<VcMinor> or newer;
  Note says what was found. }
function VcRuntimeOk(var Note: String): Boolean;
var
  Key: String;
  Installed, Major, Minor, Bld: Cardinal;
  View: Integer;
begin
  Result := False;
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64';
  if IsWin64 then View := HKLM64 else View := HKLM;
  if RegQueryDWordValue(View, Key, 'Installed', Installed) and (Installed = 1) and
     RegQueryDWordValue(View, Key, 'Major', Major) and RegQueryDWordValue(View, Key, 'Minor', Minor) then begin
    if not RegQueryDWordValue(View, Key, 'Bld', Bld) then Bld := 0;
    Note := IntToStr(Major) + '.' + IntToStr(Minor) + '.' + IntToStr(Bld);
    Result := (Major > 14) or ((Major = 14) and (Minor >= {#VcMinor}));
    if not Result then Note := Note + ', older than 14.{#VcMinor}: install the latest from {#VcUrl}';
  end else
    Note := 'not found: install it from {#VcUrl}';
end;

procedure InitializeWizard;
begin
  KeysPage := CreateInputFilePage(wpSelectDir, 'Your keys', 'Pick the prod.keys file from your own console.',
    '{#AppName} needs the prod.keys file from your own console. It is checked now and copied into the install folder only.');
  KeysPage.Add('prod.keys:', 'Key files (*.keys)|*.keys|All files|*.*', '.keys');
  KeysPage.Values[0] := Param('KEYS');

  FirmwareZipPage := CreateInputFilePage(KeysPage.ID, 'Firmware (optional)',
    'Optional. Not needed for {#GameNames}.',
    'Leave empty and press Next to skip. To install firmware anyway, pick a zip here or a folder on the next page.');
  FirmwareZipPage.Add('Firmware zip:', 'Zip files (*.zip)|*.zip', '.zip');
  { A text field, not a folder picker: Inno's folder page always demands a full path, so it could not be left empty. }
  FirmwareDirPage := CreateInputQueryPage(FirmwareZipPage.ID, 'Firmware folder (optional)',
    'Optional. Not needed for {#GameNames}. Or type the path of a folder holding the firmware files (.nca).', 'Leave empty and press Next to skip.');
  FirmwareDirPage.Add('Firmware folder:', False);
  if DirExists(Param('FIRMWARE')) then FirmwareDirPage.Values[0] := Param('FIRMWARE')
  else FirmwareZipPage.Values[0] := Param('FIRMWARE');

  GamePage := CreateInputFilePage(FirmwareDirPage.ID, 'Your game', 'Pick your own copy of a supported game (NSP or XCI): {#GameNames}.',
    'It is checked now. By default {#AppName} uses it where it is; tick the next page to copy it into the install folder.');
  GamePage.Add('Game file:', 'Game files (*.nsp;*.xci)|*.nsp;*.xci|All files|*.*', '.nsp');
  GamePage.Values[0] := Param('GAME');
  { Clean-machine test: default OFF; the page says what ticking it costs (the size is filled in when the page shows). }
  CopyPage := CreateInputOptionPage(GamePage.ID, 'Game location', 'Use the game where it is, or copy it?',
    'Leave this unticked to use the game where it is: nothing is copied.', False, False);
  CopyPage.Add('Copy the game into the install folder');
  CopyPage.Values[0] := Param('COPYGAME') = '1';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = KeysPage.ID then begin
    if not FileExists(KeysPage.Values[0]) or not KeysLookRight(KeysPage.Values[0]) then begin
      SuppressibleMsgBox('That is not a prod.keys file (it needs header_key and the key area keys). Pick your own prod.keys.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = FirmwareZipPage.ID then begin
    if (FirmwareZipPage.Values[0] <> '') and not FileExists(FirmwareZipPage.Values[0]) then begin
      SuppressibleMsgBox('That firmware zip does not exist. Pick it again or leave the field empty.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = FirmwareDirPage.ID then begin
    if (FirmwareDirPage.Values[0] <> '') and not DirExists(FirmwareDirPage.Values[0]) then begin
      SuppressibleMsgBox('That firmware folder does not exist. Type it again or leave the field empty.', mbError, MB_OK, IDOK);
      Result := False;
    end;
  end else if CurPageID = GamePage.ID then begin
    if not FileExists(GamePage.Values[0]) or (GameKind(GamePage.Values[0]) = '') then begin
      SuppressibleMsgBox('That is not an NSP or XCI game file. Pick your own copy of a supported game.', mbError, MB_OK, IDOK);
      Result := False;
    end else if Pos('no supported', GameKind(GamePage.Values[0])) > 0 then
      Result := SuppressibleMsgBox('This file does not look like a supported game ({#GameNames}). Use it anyway?', mbConfirmation, MB_YESNO, IDYES) = IDYES;
  end;
end;

{ The game's size as text, e.g. '6.8 GB' ('' if it cannot be read). }
function GameSizeText(const FileName: String): String;
var
  Size: Int64;
  Tenths: Int64;
begin
  Result := '';
  if FileSize64(FileName, Size) then begin
    Tenths := Size div 107374182;
    Result := IntToStr(Tenths div 10) + '.' + IntToStr(Tenths mod 10) + ' GB';
  end;
end;

{ A path inside a single-quoted PowerShell string: each ' doubled (C:\Users\O'Brien\... must not end the string). }
function PsQuoted(const S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '''', '''''', True);
end;

function YesNo(B: Boolean): String;
begin
  if B then Result := 'yes' else Result := 'NO';
end;

{ A title's profile files from an earlier release, except the player's own user.toml. }
procedure RemoveProfileTomls(const Dir: String);
var
  Rec: TFindRec;
begin
  if FindFirst(Dir + '\*.toml', Rec) then begin
    try
      repeat
        if CompareText(Rec.Name, 'user.toml') <> 0 then
          DeleteFile(Dir + '\' + Rec.Name);
      until not FindNext(Rec);
    finally
      FindClose(Rec);
    end;
  end;
end;

{ An upgrade: what an earlier release installed goes before this one's files are copied, so a file this release
  dropped (a patch the launcher would still stage, an old profile or script) does not stay behind. The player's
  files stay: profiles\<title>\user.toml, the saves, keys, logs and the game pointer. }
procedure RemoveOldReleaseFiles;
var
  App: String;
  Rec: TFindRec;
begin
  App := ExpandConstant('{app}');
  DelTree(App + '\tools\vizr', True, True, True);
  DelTree(App + '\patches', True, True, True);
  DelTree(App + '\LICENSES', True, True, True);
  if FindFirst(App + '\profiles\*', Rec) then begin
    try
      repeat
        if ((Rec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0) and (Rec.Name <> '.') and (Rec.Name <> '..') then begin
          DelTree(App + '\profiles\' + Rec.Name + '\patches', True, True, True);
          DelTree(App + '\profiles\' + Rec.Name + '\layouts', True, True, True);
          RemoveProfileTomls(App + '\profiles\' + Rec.Name);
        end;
      until not FindNext(Rec);
    finally
      FindClose(Rec);
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Bin, Game, Target, Fw, Cmd: String;
  Code: Integer;
  Rec: TFindRec;
begin
  if CurStep = ssInstall then RemoveOldReleaseFiles;
  if CurStep <> ssPostInstall then Exit;
  Bin := ExpandConstant('{app}\build\{#BinDir}\bin\user');
  { keys }
  ForceDirectories(Bin + '\keys');
  KeysOk := FileCopy(KeysPage.Values[0], Bin + '\keys\prod.keys', False);
  if FileExists(ExtractFilePath(KeysPage.Values[0]) + 'title.keys') then
    FileCopy(ExtractFilePath(KeysPage.Values[0]) + 'title.keys', Bin + '\keys\title.keys', False);
  { firmware }
  FirmwareOk := False;
  FirmwareNote := 'not installed (not needed for the supported games)';
  Fw := Bin + '\nand\system\Contents\registered';
  if FirmwareZipPage.Values[0] <> '' then begin
    ForceDirectories(Fw);
    Cmd := '-NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath ''' + PsQuoted(FirmwareZipPage.Values[0]) + ''' -DestinationPath ''' + PsQuoted(Fw) + ''' -Force"';
    FirmwareOk := Exec('powershell.exe', Cmd, '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
    if FirmwareOk then FirmwareNote := 'installed from ' + FirmwareZipPage.Values[0] else FirmwareNote := 'the zip could not be unpacked';
  end else if FirmwareDirPage.Values[0] <> '' then begin
    ForceDirectories(Fw);
    FirmwareOk := True;
    if FindFirst(AddBackslash(FirmwareDirPage.Values[0]) + '*.nca', Rec) then begin
      try
        repeat
          if not FileCopy(AddBackslash(FirmwareDirPage.Values[0]) + Rec.Name, Fw + '\' + Rec.Name, False) then FirmwareOk := False;
        until not FindNext(Rec);
      finally
        FindClose(Rec);
      end;
    end else FirmwareOk := False;
    if FirmwareOk then FirmwareNote := 'installed from ' + FirmwareDirPage.Values[0] else FirmwareNote := 'no .nca files copied from that folder';
  end;
  { game }
  Game := GamePage.Values[0];
  Log('{#AppName}: game ' + Game + ', copy into the install folder: ' + YesNo(CopyPage.Values[0]));
  ForceDirectories(ExpandConstant('{app}\game'));
  if CopyPage.Values[0] then begin
    { Clean-machine test: a 7 GB copy ran here for 68 s with nothing on screen and nothing in the log. }
    Target := ExpandConstant('{app}\game\') + ExtractFileName(Game);
    WizardForm.StatusLabel.Caption := 'Copying the game into the install folder (' + GameSizeText(Game) + '). This can take a few minutes...';
    WizardForm.FilenameLabel.Caption := Target;
    Log('{#AppName}: copying the game to ' + Target);
    if FileCopy(Game, Target, False) then begin
      Game := Target;
      Log('{#AppName}: game copied');
    end else begin
      Log('{#AppName}: the game copy FAILED; the original is used');
      CopyFailed := True;
    end;
  end else
    Log('{#AppName}: the game is used where it is (not copied)');
  GameOk := FileExists(Game);
  { Line 2 = the title id, for a file name without [0100...] (launch.ps1 reads it; an XCI may not name it: then
    launch.ps1 uses the only profile installed). }
  SaveStringToFile(ExpandConstant('{app}\game\GAME.txt'), Game + #13#10 + Uppercase(ReleasedTitleIn(HeadText(Game, 1048576))) + #13#10, False);
  GameNote := Game;
  if CopyFailed then
    GameNote := GameNote + ' (the copy into the install folder failed: the game is used where it is)';
  { VR runtime }
  RuntimeNote := OpenXrRuntime;
  RuntimeOk := RuntimeNote <> '';
  if not RuntimeOk then RuntimeNote := 'none found: install Virtual Desktop, SteamVR or the Oculus/Meta app before playing';
  { Visual C++ runtime }
  VcOk := VcRuntimeOk(VcNote);
  Log('{#AppName}: Visual C++ runtime ' + VcNote + ' (needs 14.{#VcMinor} or newer): ' + YesNo(VcOk));
  SaveStringToFile(ExpandConstant('{app}\INSTALL-CHECK.txt'),
    'keys: ' + IntToStr(Ord(KeysOk)) + #13#10 + 'firmware: ' + FirmwareNote + #13#10 + 'game: ' + IntToStr(Ord(GameOk)) + ' ' + GameNote + #13#10 +
    'vr runtime: ' + IntToStr(Ord(RuntimeOk)) + ' ' + RuntimeNote + #13#10 +
    'visual c++ runtime: ' + IntToStr(Ord(VcOk)) + ' ' + VcNote + #13#10, False);
end;

procedure CurPageChanged(CurPageID: Integer);
var
  SizeText: String;
begin
  if CurPageID = CopyPage.ID then begin
    SizeText := GameSizeText(GamePage.Values[0]);
    if SizeText <> '' then
      CopyPage.CheckListBox.ItemCaption[0] := 'Copy the game into the install folder (copies ' + SizeText + ')';
  end;
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      'What was checked:' + #13#10 +
      '  Keys installed: ' + YesNo(KeysOk) + #13#10 +
      '  Firmware: ' + FirmwareNote + #13#10 +
      '  Game found: ' + YesNo(GameOk) + ' (' + GameNote + ')' + #13#10 +
      '  VR runtime present: ' + YesNo(RuntimeOk) + ' (' + RuntimeNote + ')' + #13#10 +
      '  Visual C++ runtime 14.{#VcMinor} or newer: ' + YesNo(VcOk) + ' (' + VcNote + ')' + #13#10 + #13#10 +
      'Start your VR runtime, then double-click {#AppName} on the desktop.';
end;
