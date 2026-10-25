// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// VIZR MM: the player's settings in profiles\<title>\user.toml,
// read at install over game.toml and written whole from the settings table. Never game.toml.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/vizr.h"

namespace VIZR::UserToml {

/// VIZR_USER_TOML if set (the launcher points it at the LIVE profile folder), else <profile dir>/user.toml.
std::filesystem::path PathFor(const std::filesystem::path& profile_dir);

/// Applies the file's values over `settings` (missing file = nothing to apply). Keys or sections this build
/// does not know are described in `notes`, never fatal; a value outside its range is clamped and noted.
void Load(const std::filesystem::path& path, UserSettings& settings, std::vector<std::string>& notes);

/// Writes every setting from the table (the file is rewritten whole, through a temporary file).
bool Save(const std::filesystem::path& path, const UserSettings& settings, std::string& error);

} // namespace VIZR::UserToml
