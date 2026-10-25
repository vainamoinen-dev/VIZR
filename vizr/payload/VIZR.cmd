@echo off
rem SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
rem SPDX-License-Identifier: GPL-3.0-or-later
rem VIZR (build 52). Runs from this folder.
set "VIZR_ROOT=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\vizr\launch.ps1" %*
if errorlevel 1 pause