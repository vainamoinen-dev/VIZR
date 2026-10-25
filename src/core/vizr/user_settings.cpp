// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <utility>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "common/vizr_settings.h"
#include "core/vizr/user_settings.h"

namespace VIZR::UserToml {

using Settings::Scope;
using Settings::Table;

namespace {

/// Format v0.2 (R5): the v0.1 names of renamed settings ("section.new" <- "old"), read when the new key is missing.
constexpr std::array<std::pair<std::string_view, std::string_view>, 4> OldKeys{{
    {"crouch_gesture", "crouch_morph"},
    {"hand_forward_m", "cannon_forward_m"},
    {"hand_up_m", "cannon_up_m"},
    {"hand_scale", "cannon_scale"},
}};

std::string_view OldKey(std::string_view key) {
    for (const auto& [now, old] : OldKeys) {
        if (now == key) {
            return old;
        }
    }
    return {};
}

/// Format v0.2 (R9): persisted state (Scope::State, e.g. the selected visor) lives in runtime.toml beside user.toml.
/// The defaults version a save records ([meta] defaults), and the defaults changed since then with their old values.
constexpr s64 DefaultsVersion = 41;
struct DefaultChange {
    std::string_view path;
    s64 since;
    float old_value;
};
constexpr std::array<DefaultChange, 2> DefaultChanges{{
    {"compat.game_fps", 40, 60.0f}, // 60 -> Match headset
    {"compat.game_fps", 41, 0.0f},  // Match headset -> 60 again (a 90 tick felt laggy in the headset)
}};

std::filesystem::path RuntimePath(const std::filesystem::path& user_toml) {
    return user_toml.parent_path() / "runtime.toml";
}

toml::table ReadToml(const std::filesystem::path& path, std::vector<std::string>* notes, bool* unreadable = nullptr) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {};
    }
    try {
        return toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        if (notes != nullptr) {
            notes->push_back(fmt::format("{} unreadable ({}); defaults kept", path.string(), e.description()));
        }
        if (unreadable != nullptr) {
            *unreadable = true;
        }
        return {};
    }
}

/// A setting's value: a number, or (format v0.2) true / false as 1 / 0.
std::optional<double> NumberOf(const toml::node_view<const toml::node>& node) {
    if (const auto b = node.value_exact<bool>()) {
        return *b ? 1.0 : 0.0;
    }
    return node.value<double>();
}

} // namespace

std::filesystem::path PathFor(const std::filesystem::path& profile_dir) {
    if (const char* env = std::getenv("VIZR_USER_TOML"); env != nullptr && *env != '\0') {
        return std::filesystem::path{env};
    }
    return profile_dir / "user.toml";
}

void Load(const std::filesystem::path& path, UserSettings& settings, std::vector<std::string>& notes) {
    const toml::table file = ReadToml(path, &notes);
    const toml::table runtime = ReadToml(RuntimePath(path), &notes);
    std::set<std::string> known{"meta.defaults"}; // "section.key" this build reads
    // Every save writes every key, so a value the player never set looks set. A file saved under an older defaults
    // version whose value equals a changed default's old value takes the new default (once; a value the player chose
    // to equal the old default cannot be told apart).
    const s64 file_defaults = file.at_path("meta.defaults").value_or(s64{39});
    const auto apply = [&](const Settings::Spec& spec, std::size_t element) {
        const std::string section = Settings::Section(spec, settings, element);
        known.insert(section + "." + spec.key);
        // Format v0.2: state from runtime.toml (else the older user.toml [state]); a renamed key's older name.
        auto node = spec.scope == Scope::State ? runtime.at_path(section + "." + spec.key) : toml::node_view<const toml::node>{};
        if (!node) {
            node = file.at_path(section + "." + spec.key);
            if (node && spec.scope == Scope::State) {
                notes.push_back(fmt::format("{}.{}: moves to runtime.toml (format v0.2; written there on the next save)",
                                            section, spec.key));
            }
        }
        if (const auto old = OldKey(spec.key); !node && !old.empty()) {
            node = file.at_path(section + "." + std::string{old});
            known.insert(section + "." + std::string{old});
            if (node) {
                notes.push_back(fmt::format("{}.{} is now {} (format v0.2; renamed on the next save)", section, old,
                                            spec.key));
            }
        }
        if (!node) {
            return; // migration: a missing key keeps its default
        }
        const auto value = NumberOf(node);
        if (!value) {
            notes.push_back(fmt::format("{}.{} is not a number; default kept", section, spec.key));
            return;
        }
        if (!std::isfinite(*value)) { // nan / inf pass a clamp
            notes.push_back(fmt::format("{}.{} is not a finite number; default kept", section, spec.key));
            return;
        }
        float clamped = std::clamp(static_cast<float>(*value), spec.min, spec.max);
        if (clamped != static_cast<float>(*value)) {
            notes.push_back(fmt::format("{}.{} = {} outside {}..{}; clamped", section, spec.key, *value, spec.min, spec.max));
        }
        if (spec.choice_count > 0) { // a choice takes one of its values: the nearest
            float best = spec.choice_values[0];
            for (std::size_t i = 1; i < spec.choice_count; ++i) {
                if (std::abs(spec.choice_values[i] - clamped) < std::abs(best - clamped)) {
                    best = spec.choice_values[i];
                }
            }
            if (best != clamped) {
                notes.push_back(fmt::format("{}.{} = {} is not one of its choices; {} used", section, spec.key, clamped, best));
            }
            clamped = best;
        }
        for (const auto& change : DefaultChanges) {
            if (file_defaults < change.since && change.path == section + "." + spec.key && clamped == change.old_value) {
                notes.push_back(fmt::format("{}.{} = {} was the old default; the new default {} is used (defaults {})",
                                            section, spec.key, clamped, *spec.field(settings, element), change.since));
                return;
            }
        }
        *spec.field(settings, element) = clamped;
    };
    for (const auto& spec : Table) {
        if (spec.scope == Scope::Element) {
            for (std::size_t e = 0; e < settings.hud.count; ++e) {
                apply(spec, e);
            }
        } else {
            apply(spec, 0);
        }
    }
    // Keys this build does not read (another game's element, a newer build's setting): kept in mind, not fatal.
    const auto walk = [&](auto&& self, const toml::table& table, const std::string& prefix) -> void {
        for (const auto& [key, node] : table) {
            const std::string name = prefix.empty() ? std::string{key.str()} : prefix + "." + std::string{key.str()};
            if (const auto* sub = node.as_table()) {
                self(self, *sub, name);
            } else if (!known.contains(name)) {
                notes.push_back(fmt::format("{} not used by this build or game; left alone", name));
            }
        }
    };
    walk(walk, file, "");
}

bool Save(const std::filesystem::path& path, const UserSettings& settings, std::string& error) {
    // Start from the file as it is, so every key this build does not read (a newer build's
    // setting, another game's element) survives a save from an older build; then set every key from the table.
    // Format v0.2: persisted state goes to runtime.toml, and a renamed key's older name is dropped.
    // A file that does not parse is kept as <name>.bak-<time> before it is replaced, so nothing the player wrote by hand
    // is lost without a copy.
    const auto keep_unreadable = [&](const std::filesystem::path& target, toml::table& table) {
        bool unreadable = false;
        table = ReadToml(target, nullptr, &unreadable);
        // One backup per file per launch: a save that keeps failing does not leave a .bak every second.
        static std::set<std::string> backed_up;
        if (unreadable && !backed_up.contains(target.string())) {
            std::filesystem::path backup = target;
            backup += fmt::format(".bak-{}", std::chrono::system_clock::now().time_since_epoch().count());
            std::error_code ec;
            std::filesystem::copy_file(target, backup, ec);
            if (ec) {
                error = fmt::format("{} does not parse and could not be backed up ({}); not overwritten", target.string(),
                                    ec.message());
                return false;
            }
            backed_up.insert(target.string());
        }
        return true;
    };
    toml::table file, runtime;
    const auto runtime_path = RuntimePath(path);
    if (!keep_unreadable(path, file) || !keep_unreadable(runtime_path, runtime)) {
        return false;
    }
    const auto put = [&](const Settings::Spec& spec, std::size_t element) {
        const std::string section = Settings::Section(spec, settings, element);
        toml::table* table = spec.scope == Scope::State ? &runtime : &file;
        std::size_t start = 0;
        while (start <= section.size()) {
            const std::size_t dot = section.find('.', start);
            const std::string part = section.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            toml::table* next = (*table)[part].as_table();
            if (next == nullptr) {
                table->insert_or_assign(part, toml::table{});
                next = (*table)[part].as_table();
            }
            table = next;
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        UserSettings copy = settings;
        const float raw = *spec.field(copy, element);
        if (!std::isfinite(raw)) {
            return; // never write nan / inf: the file keeps its value
        }
        const double value = std::round(static_cast<double>(raw) * 1000.0) / 1000.0;
        table->insert_or_assign(spec.key, value);
        if (const auto old = OldKey(spec.key); !old.empty()) {
            table->erase(old);
        }
    };
    for (const auto& spec : Table) {
        if (spec.scope == Scope::Element) {
            for (std::size_t e = 0; e < settings.hud.count; ++e) {
                put(spec, e);
            }
        } else {
            put(spec, 0);
        }
    }
    if (file["meta"].as_table() == nullptr) {
        file.insert_or_assign("meta", toml::table{});
    }
    file["meta"].as_table()->insert_or_assign("defaults", DefaultsVersion);
    file.erase("state"); // moved to runtime.toml (format v0.2)
    // Both files are written to unique temporary names first, each checked after closing (a full disk leaves a
    // truncated temp, never a truncated settings file); only then are they swapped in.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto write = [&](const std::filesystem::path& target, const toml::table& table, const std::string& header,
                           std::filesystem::path& temp) {
        std::ostringstream body;
        body << table;
        temp = target;
        temp += fmt::format(".tmp-{}", stamp);
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) {
            error = fmt::format("cannot write {}", temp.string());
            return false;
        }
        out << header << body.str() << "\n";
        out.close();
        if (!out) {
            error = fmt::format("writing {} failed (disk full?)", temp.string());
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
        return true;
    };
    std::filesystem::path temp_settings, temp_runtime;
    if (!write(path, file,
               "# VIZR player settings (in-headset mod menu). Written by Eden from the settings table;\n"
               "# game.toml is never changed. Missing keys take their defaults; keys this build does not\n"
               "# read are kept as they are.\n\n",
               temp_settings)) {
        return false;
    }
    if (!write(runtime_path, runtime,
               "# VIZR runtime state (not settings): what the game was left at, e.g. the selected visor.\n"
               "# Written by Eden; safe to delete.\n\n",
               temp_runtime)) {
        std::error_code ec;
        std::filesystem::remove(temp_settings, ec);
        return false;
    }
    for (const auto& [temp, target] : {std::pair{temp_settings, path}, std::pair{temp_runtime, runtime_path}}) {
        std::error_code ec;
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            error = fmt::format("cannot replace {}: {}", target.string(), ec.message());
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    return true;
}

} // namespace VIZR::UserToml
