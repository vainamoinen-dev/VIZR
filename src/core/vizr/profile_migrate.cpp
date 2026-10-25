// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/vizr/profile_migrate.h"

#include <array>
#include <optional>
#include <stdexcept>
#include <string_view>

#include <fmt/format.h>

namespace VIZR::Profile {
namespace {

[[noreturn]] void MigrateFail(const std::string& where, const std::string& what) {
    throw std::runtime_error(fmt::format("{}: {}", where, what));
}

/// v0.2 "columns" vocabulary: the OpenXR direction (x right, y up, z back) each name means.
std::optional<std::array<int, 3>> NameVector(std::string_view name) {
    if (name == "right") return std::array<int, 3>{1, 0, 0};
    if (name == "left") return std::array<int, 3>{-1, 0, 0};
    if (name == "up") return std::array<int, 3>{0, 1, 0};
    if (name == "down") return std::array<int, 3>{0, -1, 0};
    if (name == "back") return std::array<int, 3>{0, 0, 1};
    if (name == "forward") return std::array<int, 3>{0, 0, -1};
    return std::nullopt;
}

/// Three column names: one of right/left, one of up/down, one of back/forward.
std::array<std::string, 3> ReadColumns(const std::string& where, const toml::array& list) {
    if (list.size() != 3) {
        MigrateFail(where, "columns are 3 names from right, left, up, down, forward, back");
    }
    std::array<std::string, 3> names{};
    std::array<bool, 3> used{};
    for (std::size_t k = 0; k < 3; ++k) {
        names[k] = list[k].value_or(std::string{});
        const auto v = NameVector(names[k]);
        if (!v) {
            MigrateFail(where, fmt::format("unknown column name '{}'", names[k]));
        }
        const std::size_t axis = (*v)[0] != 0 ? 0 : (*v)[1] != 0 ? 1 : 2;
        if (used[axis]) {
            MigrateFail(where, "columns name each direction pair (right/left, up/down, forward/back) once");
        }
        used[axis] = true;
    }
    return names;
}

/// The camera-local axis ("+x".."-z") a forward/back column gives: forward at k = +k, back at k = -k.
std::string ForwardAxis(const std::array<std::string, 3>& names) {
    static constexpr std::array<char, 3> letters{'x', 'y', 'z'};
    for (std::size_t k = 0; k < 3; ++k) {
        if (names[k] == "forward") return fmt::format("+{}", letters[k]);
        if (names[k] == "back") return fmt::format("-{}", letters[k]);
    }
    return {};
}

/// v0.1 forward_axis (+ up_axis, handedness) -> the v0.2 column names (the loader's own basis rule).
std::array<std::string, 3> ColumnsFromForward(char up_axis, bool right_handed, std::string_view forward) {
    // An axis is an optional sign and one of x, y, z; anything else ("w", "+q", "") is a profile error, not an index.
    const bool signed_axis = forward.size() == 2 && (forward[0] == '+' || forward[0] == '-');
    const char axis = forward.size() == 1 || signed_axis ? forward.back() : '\0';
    if (axis < 'x' || axis > 'z') {
        MigrateFail("[space] forward_axis", fmt::format("'{}' is not an axis (x, y or z with an optional sign)", forward));
    }
    if (up_axis < 'x' || up_axis > 'z') {
        MigrateFail("[space] up_axis", fmt::format("'{}' is not an axis (x, y or z)", up_axis));
    }
    std::array<int, 3> f{}, u{};
    const int sign = forward.starts_with('-') ? -1 : 1;
    f[static_cast<std::size_t>(axis - 'x')] = sign;
    u[static_cast<std::size_t>(up_axis - 'x')] = 1;
    const auto cross = [](const std::array<int, 3>& a, const std::array<int, 3>& b) {
        return std::array<int, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto r = right_handed ? cross(f, u) : cross(u, f);
    std::array<std::string, 3> names{"?", "?", "?"};
    for (std::size_t k = 0; k < 3; ++k) {
        if (r[k] != 0) names[k] = r[k] > 0 ? "right" : "left";
        if (u[k] != 0) names[k] = "up";
        if (f[k] != 0) names[k] = f[k] > 0 ? "forward" : "back";
    }
    return names;
}

std::string Quote(const std::array<std::string, 3>& n) {
    return fmt::format("[\"{}\", \"{}\", \"{}\"]", n[0], n[1], n[2]);
}

/// Two nodes hold the same value (node_view compares type and value).
bool Same(const toml::node& a, const toml::node& b) {
    return toml::node_view<const toml::node>{&a} == toml::node_view<const toml::node>{&b};
}

/// Moves `from` into `to` inside `table` (a v0.2 key into the loader's spelling). Both present and different = fail.
void Rename(toml::table& table, std::string_view from, std::string_view to, const std::string& where) {
    const toml::node* src = table.get(from);
    if (src == nullptr) {
        return;
    }
    if (const toml::node* dst = table.get(to); dst != nullptr && !Same(*dst, *src)) {
        MigrateFail(where, fmt::format("'{}' and the older '{}' disagree; keep one", from, to));
    }
    table.insert_or_assign(std::string{to}, *src);
    table.erase(from);
}

void Removed(const toml::table& table, std::initializer_list<std::string_view> keys, const std::string& where,
             std::vector<std::string>& notes) {
    for (const auto key : keys) {
        if (table.contains(key)) {
            notes.push_back(fmt::format("{}: '{}' is not read; remove it (format v0.2)", where, key));
        }
    }
}

void NotYet(const toml::table& table, std::initializer_list<std::string_view> keys, const std::string& where,
            std::vector<std::string>& notes) {
    for (const auto key : keys) {
        if (table.contains(key)) {
            notes.push_back(fmt::format("{}: '{}' is reserved in v0.2 and not read by this build", where, key));
        }
    }
}

void VersionNote(const toml::table& table, const std::string& file, std::vector<std::string>& notes) {
    const auto v = table["format_version"].value<int64_t>();
    if (!v || *v == 1) {
        notes.push_back(fmt::format("{}: format v0.1 (migrate: format_version = 2 and the v0.2 keys below)", file));
    } else if (*v != 2) {
        MigrateFail(file, fmt::format("format_version {} (this build reads 1 and 2)", *v));
    }
}

} // namespace

void ExpandBases(toml::table& address) {
    const toml::table* bases = address["bases"].as_table();
    if (bases == nullptr) {
        return;
    }
    // Resolve each base fully (a base may start from another; at most 8 levels, so a cycle fails).
    const auto resolve = [&](std::string text) {
        for (int depth = 0; text.starts_with("base:"); ++depth) {
            if (depth == 8) {
                MigrateFail("address file [bases]", fmt::format("'{}': a cycle or more than 8 levels", text));
            }
            const auto arrow = text.find("->");
            std::string name = text.substr(5, arrow == std::string::npos ? std::string::npos : arrow - 5);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            const auto chain = (*bases)[name].value<std::string>();
            if (!chain) {
                MigrateFail("address file", fmt::format("unknown base '{}' ([bases])", name));
            }
            text = *chain + (arrow == std::string::npos ? std::string{} : " " + text.substr(arrow));
        }
        return text;
    };
    const auto walk = [&](auto&& self, toml::node& node) -> void {
        if (auto* s = node.as_string(); s != nullptr && s->get().starts_with("base:")) {
            s->get() = resolve(s->get());
        } else if (auto* t = node.as_table()) {
            for (auto&& [key, child] : *t) {
                if (key.str() != "bases") {
                    self(self, child);
                }
            }
        } else if (auto* a = node.as_array()) {
            for (auto& child : *a) {
                self(self, child);
            }
        }
    };
    for (auto&& [key, child] : address) {
        if (key.str() != "bases") {
            walk(walk, child);
        }
    }
    address.erase("bases"); // expanded: the loader never sees it
}

void NormalizeAddress(toml::table& address, std::vector<std::string>& notes) {
    VersionNote(address, "address file", notes);
    if (const auto* hooks = address["hooks"].as_table()) {
        for (auto&& [name, node] : *hooks) {
            if (const auto* h = node.as_table(); h != nullptr && h->contains("order")) {
                notes.push_back(fmt::format("address file hook '{}': 'order' is not read; remove it (format v0.2)",
                                            name.str()));
            }
        }
    }
    if (const auto* fields = address["fields"].as_table()) {
        for (auto&& [name, node] : *fields) {
            const auto* f = node.as_table();
            const auto base = f ? (*f)["base"].value<std::string>() : std::nullopt;
            if (base && base->starts_with("ptr:")) {
                notes.push_back(fmt::format("address file field '{}': the 'ptr:' root is removed; write 'static:'",
                                            name.str()));
            }
        }
    }
    // R4: [patch_files] (v0.2) = the v0.1 address [patches].
    if (address.contains("patch_files")) {
        if (address.contains("patches")) {
            MigrateFail("address file", "[patch_files] and the older [patches] both present; keep [patch_files]");
        }
        Rename(address, "patch_files", "patches", "address file");
    } else if (address.contains("patches")) {
        notes.push_back("address file: [patches] (migrate: [patch_files])");
    }
    // R3: [aim] columns (v0.2) = ray_columns.
    if (auto* aim = address["aim"].as_table()) {
        if (aim->contains("ray_columns")) {
            notes.push_back("address file [aim]: ray_columns (migrate: columns)");
        }
        if (const auto* cols = (*aim)["columns"].as_array()) {
            ReadColumns("address file [aim] columns", *cols);
        }
        Rename(*aim, "columns", "ray_columns", "address file [aim]");
    }
    // R6: [body] <k> (v0.2) = [arms] body_<k>.
    if (auto* arms = address["arms"].as_table()) {
        std::size_t old_keys = 0;
        for (auto&& [key, node] : *arms) {
            old_keys += key.str().starts_with("body_") ? 1 : 0;
        }
        if (old_keys != 0) {
            notes.push_back(fmt::format("address file [arms]: {} body_* key(s) (migrate: [body] without the prefix)",
                                        old_keys));
        }
    }
    if (auto* body = address["body"].as_table()) {
        auto* arms = address["arms"].as_table();
        if (arms == nullptr) {
            MigrateFail("address file [body]", "needs [arms] (the body is drawn by the arm character set)");
        }
        for (auto&& [key, node] : *body) {
            const std::string to = fmt::format("body_{}", key.str());
            if (const toml::node* old = arms->get(to); old != nullptr && !Same(*old, node)) {
                MigrateFail("address file [body]", fmt::format("'{}' and the older [arms] {} disagree", key.str(), to));
            }
            arms->insert_or_assign(to, node);
        }
        address.erase("body");
    }
    // R11: [hud_source] (v0.2) = [hud_widgets]; R2: base_offset_x / _y = base_offset.
    if (address.contains("hud_widgets")) {
        notes.push_back("address file: [hud_widgets] (migrate: [hud_source], kind = \"widget_tree\")");
    }
    if (auto* source = address["hud_source"].as_table()) {
        if (address.contains("hud_widgets")) {
            MigrateFail("address file", "[hud_source] and the older [hud_widgets] both present; keep [hud_source]");
        }
        const std::string kind = (*source)["kind"].value_or(std::string{"widget_tree"});
        if (kind != "widget_tree") {
            MigrateFail("address file [hud_source]", fmt::format("kind '{}' is not supported by this build", kind));
        }
        source->erase("kind");
        if (auto* elements = (*source)["elements"].as_array()) {
            for (auto& node : *elements) {
                auto* e = node.as_table();
                if (e == nullptr || (!e->contains("base_offset_x") && !e->contains("base_offset_y"))) {
                    continue;
                }
                if (e->contains("base_offset")) {
                    MigrateFail("address file [hud_source]", "an element has base_offset_x/_y and the older base_offset");
                }
                toml::array pair;
                pair.push_back((*e)["base_offset_x"].value_or(0.0));
                pair.push_back((*e)["base_offset_y"].value_or(0.0));
                e->insert_or_assign("base_offset", std::move(pair));
                e->erase("base_offset_x");
                e->erase("base_offset_y");
            }
        }
        toml::table copy = *source;
        address.erase("hud_source");
        address.insert_or_assign("hud_widgets", std::move(copy));
    } else if (auto* widgets = address["hud_widgets"].as_table()) {
        if (const auto* elements = (*widgets)["elements"].as_array()) {
            for (const auto& node : *elements) {
                if (const auto* e = node.as_table(); e != nullptr && e->contains("base_offset")) {
                    notes.push_back("address file [hud_widgets]: base_offset = [x, y] (migrate: base_offset_x, "
                                    "base_offset_y)");
                    break;
                }
            }
        }
    }
    // R8: [fade] states moves to game.toml [render] fade_states (NormalizeGame copies it back for the loader).
    if (address.contains("fade")) {
        notes.push_back("address file: [fade] states (migrate: game.toml [render] fade_states)");
    }
}

void NormalizeGame(toml::table& game, toml::table& address, std::vector<std::string>& notes) {
    VersionNote(game, "game.toml", notes);
    // R3: [space] camera_columns (v0.2) = forward_axis with up_axis and handedness.
    std::array<std::string, 3> space_columns{"right", "up", "back"};
    if (auto* space = game["space"].as_table()) {
        const std::string where = "game.toml [space]";
        Removed(*space, {"rotation_order"}, where, notes);
        const std::string up = (*space)["up_axis"].value_or(std::string{"y"});
        const bool right_handed = (*space)["handedness"].value_or(std::string{"right"}) == "right";
        const char up_axis = up.size() == 1 && up[0] >= 'x' && up[0] <= 'z' ? up[0] : 'y';
        if (const auto* cols = (*space)["camera_columns"].as_array()) {
            space_columns = ReadColumns(where + " camera_columns", *cols);
            const std::string forward = ForwardAxis(space_columns);
            // The loader builds the same basis from forward_axis, up_axis and handedness: they must agree.
            if (ColumnsFromForward(up_axis, right_handed, forward) != space_columns) {
                MigrateFail(where, fmt::format("camera_columns {} disagree with up_axis '{}' / handedness (expected {})",
                                               Quote(space_columns), up_axis,
                                               Quote(ColumnsFromForward(up_axis, right_handed, forward))));
            }
            if (const auto old = (*space)["forward_axis"].value<std::string>(); old && *old != forward) {
                MigrateFail(where, fmt::format("camera_columns and the older forward_axis '{}' disagree", *old));
            }
            space->insert_or_assign("forward_axis", forward);
            space->erase("camera_columns");
        } else if (const auto old = (*space)["forward_axis"].value<std::string>()) {
            space_columns = ColumnsFromForward(up_axis, right_handed, *old);
            notes.push_back(fmt::format("{}: forward_axis = \"{}\" (migrate: camera_columns = {})", where, *old,
                                        Quote(space_columns)));
        }
    }
    if (auto* cameras = game["camera"].as_array()) {
        for (auto& node : *cameras) {
            auto* camera = node.as_table();
            if (camera == nullptr) {
                continue;
            }
            const std::string where = fmt::format("game.toml camera '{}'", (*camera)["id"].value_or(std::string{"?"}));
            Removed(*camera, {"shake", "pose_space"}, where, notes);
            // R3: columns (v0.2) = game_look_column (the column that is forward).
            if (const auto* cols = (*camera)["columns"].as_array()) {
                const auto names = ReadColumns(where + " columns", *cols);
                int look = -1;
                for (std::size_t k = 0; k < 3; ++k) {
                    look = names[k] == "forward" ? static_cast<int>(k) : look;
                }
                if (look < 0 && names != space_columns) {
                    MigrateFail(where, "columns without \"forward\" must equal [space] camera_columns");
                }
                if (const auto old = (*camera)["game_look_column"].value<int64_t>(); old && *old != look) {
                    MigrateFail(where, "columns and the older game_look_column disagree");
                }
                if (look >= 0) {
                    camera->insert_or_assign("game_look_column", static_cast<int64_t>(look));
                }
                camera->erase("columns");
            } else if (camera->contains("game_look_column")) {
                notes.push_back(fmt::format("{}: game_look_column (migrate: columns with \"forward\" at that column)",
                                            where));
            }
            if (const auto* cols = (*camera)["yaw_source_columns"].as_array()) {
                const std::string forward = ForwardAxis(ReadColumns(where + " yaw_source_columns", *cols));
                if (const auto old = (*camera)["yaw_source_forward"].value<std::string>(); old && *old != forward) {
                    MigrateFail(where, "yaw_source_columns and the older yaw_source_forward disagree");
                }
                camera->insert_or_assign("yaw_source_forward", forward);
                camera->erase("yaw_source_columns");
            } else if (camera->contains("yaw_source_forward")) {
                notes.push_back(fmt::format("{}: yaw_source_forward (migrate: yaw_source_columns)", where));
            }
            // R7: transition = { fade_ms, cut = { field, jump_units, jump_deg } } (v0.2) = the cut_* keys.
            if (const auto* t = (*camera)["transition"].as_table()) {
                for (const auto* old : {"cut_fade_ms", "cut_field", "cut_jump_units", "cut_jump_deg"}) {
                    if (camera->contains(old)) {
                        MigrateFail(where, fmt::format("transition and the older '{}' both present; keep transition", old));
                    }
                }
                NotYet(*t, {"when", "alpha"}, where + " transition", notes);
                toml::table copy = *t;
                if (const auto fade = copy["fade_ms"].value<int64_t>()) {
                    camera->insert_or_assign("cut_fade_ms", *fade);
                }
                if (const auto* cut = copy["cut"].as_table()) {
                    NotYet(*cut, {"blend", "blend_idle"}, where + " transition.cut", notes);
                    if (const auto f = (*cut)["field"].value<std::string>()) camera->insert_or_assign("cut_field", *f);
                    if (const auto u = (*cut)["jump_units"].value<double>()) camera->insert_or_assign("cut_jump_units", *u);
                    if (const auto d = (*cut)["jump_deg"].value<double>()) camera->insert_or_assign("cut_jump_deg", *d);
                }
                camera->erase("transition");
            } else if (camera->contains("cut_fade_ms") || camera->contains("cut_field")) {
                notes.push_back(fmt::format("{}: cut_* keys (migrate: transition = {{ fade_ms, cut = {{ field, "
                                            "jump_units, jump_deg }} }})", where));
            }
        }
    }
    if (auto* input = game["input"].as_table()) {
        Removed(*input, {"controller_type", "gyro_source", "head_drives_stick"}, "game.toml [input]", notes);
        if (const auto r = (*input)["recenter"].value<std::string>(); r && r->find("thumbstick") != std::string::npos) {
            notes.push_back("game.toml [input] recenter: the 'thumbstick' spelling is deprecated (write '_stick_')");
        }
    }
    // R8: [render] fade_states (v0.2) = the address file's [fade] states (where the loader reads them).
    if (auto* render = game["render"].as_table()) {
        NotYet(*render, {"post_effects"}, "game.toml [render]", notes);
        if (const auto* states = (*render)["fade_states"].as_array()) {
            if (const auto* fade = address["fade"].as_table(); fade != nullptr && (*fade)["states"].as_array() != nullptr &&
                                                              *(*fade)["states"].as_array() != *states) {
                MigrateFail("game.toml [render]", "fade_states and the address file's older [fade] states disagree");
            }
            toml::table fade;
            fade.insert_or_assign("states", *states);
            address.insert_or_assign("fade", std::move(fade));
            render->erase("fade_states");
        }
    }
    // R2: [hud.<element>] offset_x / offset_y (v0.2) = offset = [x, y].
    if (auto* hud = game["hud"].as_table()) {
        for (auto&& [name, node] : *hud) {
            auto* e = node.as_table();
            if (e == nullptr) {
                continue;
            }
            const std::string where = fmt::format("game.toml [hud.{}]", name.str());
            if (e->contains("offset_x") || e->contains("offset_y")) {
                if (e->contains("offset")) {
                    MigrateFail(where, "offset_x/_y and the older offset both present; keep offset_x/_y");
                }
                toml::array pair;
                pair.push_back((*e)["offset_x"].value_or(0.0));
                pair.push_back((*e)["offset_y"].value_or(0.0));
                e->insert_or_assign("offset", std::move(pair));
                e->erase("offset_x");
                e->erase("offset_y");
            } else if (e->contains("offset")) {
                notes.push_back(fmt::format("{}: offset = [x, y] (migrate: offset_x, offset_y)", where));
            }
        }
    }
    NotYet(game, {"audio"}, "game.toml", notes);
}

} // namespace VIZR::Profile
