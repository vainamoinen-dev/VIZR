// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include <toml++/toml.hpp>

namespace VIZR::Profile {

/// Profile format v0.2. The loader reads both v0.1 and v0.2 files: this pass runs on the
/// parsed tables before the loader and brings every v0.2 key into the form the loader reads, and reports every v0.1
/// key a v0.2 file would write differently (one note each, "migrate: ..."), every key the format removed ("... is not
/// read; remove it") and every v0.2 key the core does not act on yet. A conflict (a v0.1 and a v0.2 spelling of the same
/// key with different values, a camera_columns that disagrees with up_axis / handedness) fails the profile.
void NormalizeGame(toml::table& game, toml::table& address, std::vector<std::string>& notes);
void NormalizeAddress(toml::table& address, std::vector<std::string>& notes);

/// [bases] (v0.2 2.3): every string value in the address file that starts with "base:<name>" gets that base's chain
/// (bases may start from other bases; a cycle or an unknown name fails).
void ExpandBases(toml::table& address);

} // namespace VIZR::Profile
