// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Profiles are read with toml++ (MIT, Mark Gillard and contributors); see THIRD-PARTY-VIZR.md.

#include <algorithm>
#include <cmath>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "common/vizr_settings.h"
#include "core/vizr/profile.h"
#include "core/vizr/profile_migrate.h"

namespace VIZR::Profile {
namespace {

using VIZR::ProTarget;
using VIZR::TouchInput;

/// Profile errors carry the file and key so a contributor can fix the profile from the log.
struct ProfileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void Fail(const std::string& where, const std::string& what) {
    throw ProfileError(fmt::format("{}: {}", where, what));
}

std::string Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

/// "0x1A4", "-0x8", "16" (strings such as module offsets inside "main+0x...").
std::optional<s64> ParseInt(std::string_view text) {
    std::string s = Trim(text);
    bool negative = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        negative = s[0] == '-';
        s.erase(0, 1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.erase(0, 2);
    }
    u64 value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || end != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    // A value past s64 is an error, not a wrap (and -2^63 is the one negative without a positive twin).
    constexpr u64 max_positive = static_cast<u64>(std::numeric_limits<s64>::max());
    if (value > max_positive + (negative ? 1 : 0)) {
        return std::nullopt;
    }
    if (negative) {
        return value == max_positive + 1 ? std::numeric_limits<s64>::min() : -static_cast<s64>(value);
    }
    return static_cast<s64>(value);
}

/// "main+0x20ee268" -> {"main", 0x20ee268}.
std::pair<std::string, u64> ParseModuleAddress(const std::string& where, std::string_view text) {
    const auto plus = text.find('+');
    if (plus == std::string_view::npos) {
        Fail(where, fmt::format("'{}' is not <module>+0x<offset>", text));
    }
    const auto offset = ParseInt(text.substr(plus + 1));
    if (!offset || *offset < 0) {
        Fail(where, fmt::format("bad offset in '{}'", text));
    }
    return {Trim(text.substr(0, plus)), static_cast<u64>(*offset)};
}

std::vector<u8> ParseBytes(const std::string& where, std::string_view text) {
    std::vector<u8> bytes;
    std::string digits;
    for (const char c : text) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            digits += c;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            Fail(where, fmt::format("bad byte string '{}'", text));
        }
    }
    if (digits.size() % 2 != 0) {
        Fail(where, fmt::format("odd number of hex digits in '{}'", text));
    }
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        bytes.push_back(static_cast<u8>(std::stoul(digits.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

FieldType ParseType(const std::string& where, const std::string& text) {
    static constexpr std::array<std::pair<std::string_view, FieldType>, 11> types{{
        {"u8", FieldType::U8},     {"u16", FieldType::U16},       {"u32", FieldType::U32},
        {"u64", FieldType::U64},   {"ptr", FieldType::Ptr},       {"f32", FieldType::F32},
        {"vec3f", FieldType::Vec3f}, {"quat", FieldType::Quat},   {"mat3x4", FieldType::Mat3x4},
        {"mat4", FieldType::Mat4},
        {"addr", FieldType::Addr},
    }};
    for (const auto& [name, type] : types) {
        if (name == text) {
            return type;
        }
    }
    Fail(where, fmt::format("unknown type '{}'", text));
}

template <typename T>
T Required(const toml::table& table, std::string_view key, const std::string& where) {
    const auto value = table[key].value<T>();
    if (!value) {
        Fail(where, fmt::format("missing or wrong-typed key '{}'", key));
    }
    return *value;
}

/// Integers may be written as TOML integers or as strings ("0x1A4").
std::optional<s64> OptionalInt(const toml::table& table, std::string_view key,
                               const std::string& where) {
    const auto node = table[key];
    if (!node) {
        return std::nullopt;
    }
    if (const auto i = node.value<s64>()) {
        return *i;
    }
    if (const auto s = node.value<std::string>()) {
        if (const auto parsed = ParseInt(*s)) {
            return parsed;
        }
    }
    Fail(where, fmt::format("key '{}' is not an integer", key));
}

/// An optional unsigned key (an offset, a register, a node index): missing = `def`; negative, too large for the field,
/// or not an integer = a loud profile error (never a silent 0 or a wrapped huge offset).
u64 OptUnsigned(const toml::table& table, std::string_view key, const std::string& where, u64 max, u64 def = 0) {
    const auto v = OptionalInt(table, key, where);
    if (!v) {
        return def;
    }
    if (*v < 0 || static_cast<u64>(*v) > max) {
        Fail(where, fmt::format("key '{}' = {} is out of range (0..{:#x})", key, *v, max));
    }
    return static_cast<u64>(*v);
}
u64 OptU64(const toml::table& table, std::string_view key, const std::string& where) {
    return OptUnsigned(table, key, where, std::numeric_limits<s64>::max());
}
u32 OptU32(const toml::table& table, std::string_view key, const std::string& where) {
    return static_cast<u32>(OptUnsigned(table, key, where, std::numeric_limits<u32>::max()));
}

/// An array entry that must be an unsigned integer (a number, or a "0x..." string) no larger than `max`.
u64 ElemUnsigned(const toml::node& node, const std::string& where, u64 max) {
    std::optional<s64> v = node.value<s64>();
    if (!v) {
        if (const auto text = node.value<std::string>()) {
            v = ParseInt(*text);
        }
    }
    if (!v || *v < 0 || static_cast<u64>(*v) > max) {
        Fail(where, "an array entry is not an integer in range (0.." + fmt::format("{:#x}", max) + ")");
    }
    return static_cast<u64>(*v);
}
u64 ElemU64(const toml::node& node, const std::string& where) {
    return ElemUnsigned(node, where, std::numeric_limits<s64>::max());
}
u32 ElemU32(const toml::node& node, const std::string& where) {
    return static_cast<u32>(ElemUnsigned(node, where, std::numeric_limits<u32>::max()));
}

/// "reg:x20 -> +0x4e0168 -> +0x8", "static:main+0x1234", "sym:gpGameState -> +0x0".
void ParseBase(const std::string& where, const std::string& text, Field& field) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto arrow = text.find("->", start);
        parts.push_back(Trim(std::string_view{text}.substr(start, arrow - start)));
        if (arrow == std::string::npos) {
            break;
        }
        start = arrow + 2;
    }
    const std::string& root = parts[0];
    if (root.starts_with("reg:")) {
        const std::string reg = root.substr(4);
        if (reg == "sp") {
            field.root = RootKind::Sp;
        } else {
            const auto index = reg.size() > 1 && reg[0] == 'x' ? ParseInt(reg.substr(1)) : std::nullopt;
            if (!index || *index < 0 || *index > 30) {
                Fail(where, fmt::format("bad register '{}'", reg));
            }
            field.root = RootKind::Reg;
            field.reg = static_cast<u32>(*index);
        }
    } else if (root.starts_with("static:") || root.starts_with("ptr:")) {
        const auto [module, offset] =
            ParseModuleAddress(where, std::string_view{root}.substr(root.find(':') + 1));
        field.root = RootKind::Static;
        field.module = module;
        field.module_offset = offset;
    } else if (root.starts_with("sym:")) {
        field.root = RootKind::Sym;
        field.sym = Trim(std::string_view{root}.substr(4));
    } else {
        Fail(where, fmt::format("unknown base root '{}'", root));
    }
    for (std::size_t i = 1; i < parts.size(); ++i) {
        const auto step = ParseInt(parts[i]);
        if (!step) {
            Fail(where, fmt::format("bad chain step '{}'", parts[i]));
        }
        field.chain.push_back(*step);
    }
}

When ParseWhen(const std::string& where, const toml::table& table) {
    When when;
    if (table["any_of"].is_array()) {
        when.op = When::Op::AnyOf;
    } else if (table["all_of"].is_array()) {
        when.op = When::Op::AllOf;
    }
    if (when.op == When::Op::AnyOf || when.op == When::Op::AllOf) {
        const auto* list = table[when.op == When::Op::AnyOf ? "any_of" : "all_of"].as_array();
        for (const auto& item : *list) {
            const auto* child = item.as_table();
            if (child == nullptr) {
                Fail(where, "any_of/all_of entries must be tables");
            }
            when.children.push_back(ParseWhen(where, *child));
        }
        return when;
    }
    if (const auto* inner = table["not"].as_table()) {
        when.op = When::Op::Not;
        when.children.push_back(ParseWhen(where, *inner));
        return when;
    }
    if (const auto state = table["state"].value<std::string>()) {
        when.op = When::Op::State; // format v0.2: resolved to the state's bit once [states] is read
        when.field = *state;
        return when;
    }
    when.field = Required<std::string>(table, "field", where);
    if (table.contains("equals")) {
        when.op = When::Op::Equals;
        when.value = static_cast<u64>(*OptionalInt(table, "equals", where));
    } else if (const auto* in = table["in"].as_array()) {
        when.op = When::Op::In;
        for (const auto& item : *in) {
            const auto v = item.value<s64>();
            if (!v) {
                Fail(where, "'in' entries must be integers");
            }
            when.values.push_back(static_cast<u64>(*v));
        }
    } else if (table.contains("mask")) {
        when.op = When::Op::Mask;
        when.mask = static_cast<u64>(*OptionalInt(table, "mask", where));
        when.set = table["set"].value_or(true);
    } else if (const auto addr = table["equals_addr"].value<std::string>()) {
        when.op = When::Op::EqualsAddr;
        std::tie(when.addr_module, when.addr_offset) = ParseModuleAddress(where, *addr);
    } else if (const auto other = table["equals_field"].value<std::string>()) {
        when.op = When::Op::EqualsField;
        when.other_field = *other;
    } else if (table["is_null"].value_or(false)) {
        when.op = When::Op::IsNull;
    } else {
        Fail(where, fmt::format("no condition for field '{}'", when.field));
    }
    return when;
}

/// "-z" -> unit vector.
std::array<float, 3> AxisVector(const std::string& where, std::string_view axis) {
    float sign = 1.0f;
    if (!axis.empty() && (axis[0] == '-' || axis[0] == '+')) {
        sign = axis[0] == '-' ? -1.0f : 1.0f;
        axis.remove_prefix(1);
    }
    if (axis.size() != 1 || axis[0] < 'x' || axis[0] > 'z') {
        Fail(where, fmt::format("bad axis '{}'", axis));
    }
    std::array<float, 3> v{};
    v[static_cast<std::size_t>(axis[0] - 'x')] = sign;
    return v;
}

std::array<float, 3> Cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Space ParseSpace(const std::string& where, const toml::table& table) {
    Space space;
    const std::string up = Required<std::string>(table, "up_axis", where);
    if (up.size() != 1 || up[0] < 'x' || up[0] > 'z') {
        Fail(where, fmt::format("bad up_axis '{}'", up));
    }
    space.up_axis = up[0];
    space.right_handed = Required<std::string>(table, "handedness", where) == "right";
    space.units_per_meter = static_cast<float>(table["units_per_meter"].value_or(1.0));
    space.row_major = table["matrix_layout"].value_or(std::string{"row_major"}) == "row_major";
    const std::string sense = table["rotation_sense"].value_or(std::string{"camera_to_world"});
    if (sense != "camera_to_world" && sense != "world_to_view" && sense != "look_at") {
        Fail(where, fmt::format("rotation_sense '{}' (use camera_to_world, world_to_view or look_at)", sense));
    }
    space.look_at = sense == "look_at";
    space.camera_to_world = sense != "world_to_view";
    space.forward_axis = table["forward_axis"].value_or(std::string{"-z"});
    if (space.look_at && space.forward_axis != "-z") {
        Fail(where, "rotation_sense look_at builds its camera with forward -z; forward_axis must be -z");
    }

    // Camera-local basis: OpenXR up -> up_axis, OpenXR forward (-Z) ->
    // forward_axis, OpenXR right -> forward x up (right-handed) or up x forward (left-handed).
    const auto up_v = AxisVector(where, up);
    const auto forward_v = AxisVector(where, space.forward_axis);
    if (up_v == forward_v || up_v == std::array<float, 3>{-forward_v[0], -forward_v[1], -forward_v[2]}) {
        Fail(where, "forward_axis must differ from up_axis");
    }
    const auto right_v = space.right_handed ? Cross(forward_v, up_v) : Cross(up_v, forward_v);
    const std::array<float, 3> back_v{-forward_v[0], -forward_v[1], -forward_v[2]};
    space.local_basis = {right_v, up_v, back_v};
    return space;
}

toml::table ParseFile(const std::filesystem::path& path) {
    try {
        return toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        Fail(path.filename().string(),
             fmt::format("TOML error at line {}: {}", e.source().begin.line, e.description()));
    }
}

/// "left_thumbstick_click" and "left_stick_click" name the same Touch input.
std::string NormalizeTouchName(std::string name) {
    if (const auto at = name.find("thumbstick"); at != std::string::npos) {
        name.replace(at, 10, "stick");
    }
    return name;
}

std::optional<TouchInput> TouchByName(const std::string& raw) {
    static constexpr std::array<std::pair<std::string_view, TouchInput>, 21> names{{
        {"left_x", TouchInput::LeftX},
        {"left_y", TouchInput::LeftY},
        {"left_menu", TouchInput::LeftMenu},
        {"left_stick_click", TouchInput::LeftStickClick},
        {"left_trigger", TouchInput::LeftTrigger},
        {"left_grip", TouchInput::LeftGrip},
        {"right_a", TouchInput::RightA},
        {"right_b", TouchInput::RightB},
        {"right_stick_click", TouchInput::RightStickClick},
        {"right_trigger", TouchInput::RightTrigger},
        {"right_grip", TouchInput::RightGrip},
        {"right_stick_up", TouchInput::RightStickUp},
        {"right_stick_down", TouchInput::RightStickDown},
        {"right_stick_left", TouchInput::RightStickLeft},
        {"right_stick_right", TouchInput::RightStickRight},
        {"left_stick_up", TouchInput::LeftStickUp},
        {"left_stick_down", TouchInput::LeftStickDown},
        {"left_stick_left", TouchInput::LeftStickLeft},
        {"left_stick_right", TouchInput::LeftStickRight},
        {"left_hand_head", TouchInput::LeftHandHead},
        {"left_hand_cannon", TouchInput::LeftHandCannon},
    }};
    const std::string name = NormalizeTouchName(raw);
    for (const auto& [n, input] : names) {
        if (n == name) {
            return input;
        }
    }
    return std::nullopt;
}

std::optional<ProTarget> ProByName(const std::string& name) {
    static constexpr std::array<std::pair<std::string_view, ProTarget>, 18> names{{
        {"a", ProTarget::A},           {"b", ProTarget::B},
        {"x", ProTarget::X},           {"y", ProTarget::Y},
        {"l", ProTarget::L},           {"r", ProTarget::R},
        {"zl", ProTarget::ZL},         {"zr", ProTarget::ZR},
        {"plus", ProTarget::Plus},     {"minus", ProTarget::Minus},
        {"lstick_click", ProTarget::LStickClick}, {"rstick_click", ProTarget::RStickClick},
        {"dpad_up", ProTarget::DUp},   {"dpad_down", ProTarget::DDown},
        {"dpad_left", ProTarget::DLeft}, {"dpad_right", ProTarget::DRight},
        {"recenter", ProTarget::Recenter}, {"none", ProTarget::None},
    }};
    for (const auto& [n, target] : names) {
        if (n == name) {
            return target;
        }
    }
    return std::nullopt;
}

} // namespace

VIZR::InputLayout LoadLayout(const std::filesystem::path& path, const std::string& recenter,
                                 std::string& error, std::vector<std::string>& warnings,
                                 const std::vector<std::string>& state_names) {
    InputLayout layout;
    layout.map.fill(ProTarget::None);
    layout.chord.fill(ProTarget::None);
    layout.chord2.fill(ProTarget::None);
    try {
        const toml::table file = ParseFile(path);
        const std::string where = path.filename().string();
        // Format v0.2: a v0.1 layout and the deprecated "thumbstick" spelling are notes, never a failure.
        if (file["format_version"].value_or(int64_t{1}) == 1) {
            warnings.push_back(fmt::format("{}: format v0.1 (migrate: format_version = 2)", where));
        }
        std::size_t thumbstick = 0;
        const auto count = [&](auto&& self, const toml::table& t) -> void {
            for (const auto& [key, node] : t) {
                thumbstick += key.str().find("thumbstick") != std::string_view::npos ? 1 : 0;
                if (const auto s = node.value<std::string>()) {
                    thumbstick += s->find("thumbstick") != std::string::npos ? 1 : 0;
                } else if (const auto* sub = node.as_table()) {
                    self(self, *sub);
                }
            }
        };
        count(count, file);
        if (thumbstick != 0) {
            warnings.push_back(fmt::format("{}: {} 'thumbstick' name(s) (deprecated; write '_stick_', e.g. "
                                           "left_stick_click)", where, thumbstick));
        }
        // An entry this build cannot read is a warning and is skipped, never a dead layout (no input at all in the
        // headset is the worst failure).
        const auto read_table = [&](const toml::table* table, const std::string& section, auto& out) {
            if (table == nullptr) {
                return;
            }
            for (const auto& [key, node] : *table) {
                const std::string source{key.str()};
                if (section == "map" && (source == "left_stick" || source == "right_stick")) {
                    continue; // sticks always drive the matching Pro stick
                }
                if (node.is_table()) {
                    continue; // sub-tables ([map.menu], [chords.<name>]) are read below
                }
                if (section != "map" &&
                    (source == "modifier" || source == "modifier_mode" || source == "hold_also" ||
                     source == "flick_max_ms" || node.is_table())) {
                    continue; // read below
                }
                const auto input = TouchByName(source);
                const auto target = ProByName(node.value_or(std::string{}));
                if (!input || !target) {
                    warnings.push_back(fmt::format("{} [{}] {} = '{}': unknown {}; skipped", where, section, source,
                                                   node.value_or(std::string{}), input ? "target" : "Touch input"));
                    continue;
                }
                out[static_cast<std::size_t>(*input)] = *target;
            }
        };
        read_table(file["map"].as_table(), "map", layout.map);
        // Hotfix: [map.menu] = the targets while the game is in a menu.
        layout.map_menu.fill(ProTarget::None);
        if (const auto* menu = file["map"]["menu"].as_table()) {
            read_table(menu, "map.menu", layout.map_menu);
            for (const auto& [key, node] : *menu) {
                if (const auto input = TouchByName(std::string{key.str()}); input && ProByName(node.value_or(std::string{}))) {
                    layout.menu_override[static_cast<std::size_t>(*input)] = true;
                }
            }
        }
        // [when] source = ["state",...]: the source's own [map] target only while a state holds.
        if (const auto* when = file["when"].as_table()) {
            for (const auto& [key, node] : *when) {
                const auto input = TouchByName(std::string{key.str()});
                const auto* list = node.as_array();
                if (!input || list == nullptr) {
                    warnings.push_back(fmt::format("{} [when] {}: unknown Touch input or not a list; skipped", where,
                                                   key.str()));
                    continue;
                }
                u32 mask = 0;
                for (const auto& item : *list) {
                    const std::string state = item.value_or(std::string{});
                    const auto it = std::ranges::find(state_names, state);
                    if (it == state_names.end()) {
                        warnings.push_back(fmt::format("{} [when] {}: state '{}' is not in the address file's [states]; "
                                                       "ignored", where, key.str(), state));
                        continue;
                    }
                    mask |= 1u << static_cast<u32>(it - state_names.begin());
                }
                layout.when[static_cast<std::size_t>(*input)] = mask;
            }
        }
        // [gestures] crouch_press = target, crouch_state = "state", ring = {up, right, down, left}.
        if (const auto* gestures = file["gestures"].as_table()) {
            if (const auto press = (*gestures)["crouch_press"].value<std::string>()) {
                if (const auto target = ProByName(*press)) {
                    layout.crouch_press = *target;
                } else {
                    warnings.push_back(fmt::format("{} [gestures] crouch_press '{}': unknown target; skipped", where, *press));
                }
            }
            if (const auto state = (*gestures)["crouch_state"].value<std::string>()) {
                const auto it = std::ranges::find(state_names, *state);
                if (it == state_names.end()) {
                    warnings.push_back(fmt::format("{} [gestures] crouch_state '{}' is not in the address file's [states]; "
                                                   "the crouch keeps its own toggle", where, *state));
                } else {
                    layout.crouch_state = 1u << static_cast<u32>(it - state_names.begin());
                }
            }
            if (const auto block = (*gestures)["crouch_block"].value<std::string>()) {
                const auto it = std::ranges::find(state_names, *block);
                if (it == state_names.end()) {
                    warnings.push_back(fmt::format("{} [gestures] crouch_block '{}' is not in the address file's [states]; "
                                                   "ignored", where, *block));
                } else {
                    layout.crouch_block = 1u << static_cast<u32>(it - state_names.begin());
                }
            }
            constexpr std::array<std::string_view, 4> ring_dirs{"up", "right", "down", "left"};
            if (const auto* press = (*gestures)["ring_press"].as_table()) {
                for (std::size_t d = 0; d < ring_dirs.size(); ++d) {
                    if (const auto name = (*press)[ring_dirs[d]].value<std::string>()) {
                        if (const auto target = ProByName(*name)) {
                            layout.ring_press[d] = *target;
                        } else {
                            warnings.push_back(fmt::format("{} [gestures] ring_press {} '{}': unknown target", where,
                                                           ring_dirs[d], *name));
                        }
                    }
                }
            }
            if (const auto* slots = (*gestures)["ring_slots"].as_table()) {
                for (std::size_t d = 0; d < ring_dirs.size(); ++d) {
                    layout.ring_slots[d] = static_cast<s32>((*slots)[ring_dirs[d]].value_or(s64{-1}));
                }
            }
            if (const auto* owned = (*gestures)["ring_owned"].as_table()) {
                for (std::size_t d = 0; d < ring_dirs.size(); ++d) {
                    if (const auto state = (*owned)[ring_dirs[d]].value<std::string>()) {
                        const auto it = std::ranges::find(state_names, *state);
                        if (it == state_names.end()) {
                            warnings.push_back(fmt::format("{} [gestures] ring_owned {} '{}' is not in the address file's "
                                                           "[states]; shown always", where, ring_dirs[d], *state));
                        } else {
                            layout.ring_owned[d] = 1u << static_cast<u32>(it - state_names.begin());
                        }
                    }
                }
            }
            if (const auto on = (*gestures)["ring_state"].as_table()) {
                for (std::size_t d = 0; d < ring_dirs.size(); ++d) {
                    if (const auto state = (*on)[ring_dirs[d]].value<std::string>()) {
                        const auto it = std::ranges::find(state_names, *state);
                        if (it == state_names.end()) {
                            warnings.push_back(fmt::format("{} [gestures] ring_state {} '{}' is not in the address file's "
                                                           "[states]; that visor is never 'on'", where, ring_dirs[d], *state));
                        } else {
                            layout.ring_state[d] = 1u << static_cast<u32>(it - state_names.begin());
                        }
                    }
                }
            }
            if (const auto home = (*gestures)["ring_home"].value<std::string>()) {
                const auto it = std::ranges::find(ring_dirs, std::string_view{*home});
                layout.ring_home = it == ring_dirs.end() ? 0 : static_cast<s32>(it - ring_dirs.begin());
            }
            if (const auto state = (*gestures)["home_state"].value<std::string>()) {
                const auto it = std::ranges::find(state_names, *state);
                if (it == state_names.end()) {
                    warnings.push_back(fmt::format("{} [gestures] home_state '{}' is not in the address file's [states]; "
                                                   "a head tap always goes home", where, *state));
                } else {
                    layout.home_state = 1u << static_cast<u32>(it - state_names.begin());
                }
            }
            if (const auto* ring = (*gestures)["ring"].as_table()) {
                constexpr std::array<std::string_view, 4> dirs{"up", "right", "down", "left"};
                for (std::size_t d = 0; d < dirs.size(); ++d) {
                    const std::string label = (*ring)[dirs[d]].value_or(std::string{});
                    std::strncpy(layout.ring_labels[d].data(), label.c_str(), layout.ring_labels[d].size() - 1);
                }
            }
        }
        read_table(file["chords"].as_table(), "chords", layout.chord);
        // Second chord set: the first [chords.<name>] sub-table with a known modifier.
        if (const auto* chords = file["chords"].as_table()) {
            for (const auto& [key, node] : *chords) {
                const auto* sub = node.as_table();
                if (sub == nullptr) {
                    continue;
                }
                const std::string section = fmt::format("chords.{}", key.str());
                const auto modifier = TouchByName((*sub)["modifier"].value_or(std::string{}));
                if (layout.modifier2 != TouchInput::Count || !modifier) {
                    warnings.push_back(fmt::format("{} [{}]: {}; skipped", where, section,
                                                   modifier ? "only one extra chord set is supported"
                                                            : "missing or unknown modifier"));
                    continue;
                }
                if ((*sub)["modifier_mode"].value_or(std::string{"hold"}) != "hold") {
                    warnings.push_back(fmt::format("{} [{}]: only modifier_mode \"hold\"; using hold", where, section));
                }
                layout.modifier2 = *modifier;
                if (const auto also = (*sub)["hold_also"].value<std::string>()) {
                    if (const auto target = ProByName(*also)) {
                        layout.hold_also2 = *target;
                    } else {
                        warnings.push_back(fmt::format("{} [{}] hold_also '{}': unknown target; skipped", where,
                                                       section, *also));
                    }
                }
                read_table(sub, section, layout.chord2);
            }
        }
        if (const auto modifier = file["chords"]["modifier"].value<std::string>()) {
            const auto input = TouchByName(*modifier);
            if (!input) {
                warnings.push_back(fmt::format("{} [chords] unknown modifier '{}'; chords skipped", where, *modifier));
                layout.chord.fill(ProTarget::None);
            }
            const std::string mode = file["chords"]["modifier_mode"].value_or(std::string{"tap"});
            if (input && mode != "tap" && mode != "hold") {
                warnings.push_back(fmt::format("{} [chords] modifier_mode '{}' unknown; using \"hold\"", where, mode));
            }
            layout.modifier = input.value_or(TouchInput::Count);
            layout.modifier_hold = mode != "tap";
        }
        if (layout.modifier != TouchInput::Count && !layout.modifier_hold) {
            // The modifier's own [map] target becomes a tap sent on release without a chord.
            const auto m = static_cast<std::size_t>(layout.modifier);
            layout.modifier_tap = layout.map[m];
            layout.map[m] = ProTarget::None;
        }
        layout.deadzone = static_cast<float>(file["sticks"]["deadzone"].value_or(0.12));
        if (const auto ms = file["chords"]["flick_max_ms"].value<s64>()) {
            if (*ms < 50 || *ms > 1000) {
                warnings.push_back(fmt::format("{} [chords] flick_max_ms {} outside 50..1000; using 250", where, *ms));
            } else {
                layout.flick_max_ms = static_cast<u32>(*ms);
            }
        }
        // The profile's recenter binding wins over the layout file.
        if (const auto source = TouchByName(recenter)) {
            layout.map[static_cast<std::size_t>(*source)] = ProTarget::Recenter;
        } else {
            warnings.push_back(fmt::format("game.toml [input] recenter '{}' is not a Touch input; no recenter button",
                                           recenter));
        }
        layout.valid = true;
    } catch (const ProfileError& e) {
        error = e.what();
        layout.valid = false;
    } catch (const std::exception& e) {
        error = fmt::format("layout load failed: {}", e.what());
        layout.valid = false;
    }
    return layout;
}

const Field* GameProfile::FindField(std::string_view field_name) const {
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [&](const Field& f) { return f.name == field_name; });
    return it == fields.end() ? nullptr : &*it;
}

std::optional<GameProfile> Load(const std::filesystem::path& profiles_root,
                                const std::string& title_id, const std::string& build_id,
                                std::string& error) {
    error.clear();
    GameProfile profile;
    profile.dir = profiles_root / title_id;
    profile.title_id = title_id;
    profile.build_id = build_id;
    const auto game_path = profile.dir / "game.toml";
    const auto address_path = profile.dir / (build_id + ".toml");
    std::error_code ec;
    if (!std::filesystem::exists(game_path, ec)) {
        return std::nullopt; // no profile for this game: not an error
    }
    if (!std::filesystem::exists(address_path, ec)) {
        error = fmt::format("{} has no address file for this build ({})", profile.dir.string(),
                            address_path.filename().string());
        return std::nullopt;
    }
    try {
        toml::table game = ParseFile(game_path);
        toml::table address = ParseFile(address_path);
        // Format v0.2: v0.2 keys into the loader's form, v0.1 keys reported (one note each).
        ExpandBases(address);
        NormalizeAddress(address, profile.notes);
        NormalizeGame(game, address, profile.notes);

        profile.name = game["game"]["name"].value_or(std::string{});
        const auto* space = game["space"].as_table();
        if (space == nullptr) {
            Fail("game.toml", "missing [space]");
        }
        profile.space = ParseSpace("game.toml [space]", *space);

        if (const auto* cameras = game["camera"].as_array()) {
            for (const auto& node : *cameras) {
                const auto* table = node.as_table();
                if (table == nullptr) {
                    Fail("game.toml", "[[camera]] entries must be tables");
                }
                Camera camera;
                camera.id = Required<std::string>(*table, "id", "game.toml [[camera]]");
                const std::string where = fmt::format("game.toml camera '{}'", camera.id);
                camera.mode = Required<std::string>(*table, "mode", where);
                if (const auto* when = (*table)["when"].as_table()) {
                    camera.when = ParseWhen(where, *when);
                }
                camera.combine = (*table)["combine"].value_or(std::string{"none"});
                camera.game_pitch_cap_deg = static_cast<float>((*table)["game_pitch_cap_deg"].value_or(0.0));
                camera.game_look_column = static_cast<int>((*table)["game_look_column"].value_or(s64{-1}));
                if (camera.game_look_column > 2) {
                    Fail(where, "game_look_column must be -1, 0, 1 or 2");
                }
                // { pos, at, up } triples for a look-at camera, else one matrix field name.
                const auto triple = [&](const char* key, std::string& pos, std::string& at, std::string& up) {
                    if (const auto* t = (*table)[key].as_table()) {
                        pos = Required<std::string>(*t, "pos", where);
                        at = Required<std::string>(*t, "at", where);
                        up = Required<std::string>(*t, "up", where);
                        return true;
                    }
                    pos = (*table)[key].value_or(std::string{});
                    return false;
                };
                const bool src_triple = triple("source", camera.source, camera.source_at, camera.source_up);
                const bool dst_triple = triple("target", camera.target, camera.target_at, camera.target_up);
                if (src_triple != dst_triple) {
                    Fail(where, "source and target must both be { pos, at, up } or both one field");
                }
                camera.look_at = src_triple;
                if (camera.mode != "passthrough" && camera.look_at != profile.space.look_at) {
                    Fail(where, camera.look_at ? "a { pos, at, up } camera needs [space] rotation_sense = \"look_at\""
                                               : "rotation_sense = \"look_at\" needs source/target = { pos, at, up }");
                }
                camera.hook_silence_frames =
                    static_cast<u32>(OptionalInt(*table, "hook_silence_frames", where).value_or(3));
                camera.pose_lag_frames =
                    OptU32(*table, "pose_lag_frames", where);
                if (const auto* at = (*table)["pose_lag_frames_at"].as_table()) {
                    for (const auto& [key, lag_node] : *at) {
                        const auto hz = std::strtoul(std::string{key.str()}.c_str(), nullptr, 10);
                        const auto frames = lag_node.value<s64>();
                        if (hz < 30 || hz > 240 || !frames || *frames < 0 || *frames > 8) {
                            Fail(where, fmt::format("pose_lag_frames_at.{}: a tick rate (Hz) = frames (0-8)", key.str()));
                        }
                        camera.pose_lag_frames_at[static_cast<u32>(hz)] = static_cast<u32>(*frames);
                    }
                }
                if (const auto* fov = (*table)["fov"].as_table()) {
                    camera.fov_target = (*fov)["target"].value_or(std::string{});
                    camera.fov_kind = (*fov)["kind"].value_or(std::string{"rad_vertical"});
                    camera.fov_mode = (*fov)["mode"].value_or(std::string{"headset"});
                    camera.fov_max_deg = static_cast<float>((*fov)["max_deg"].value_or(110.0));
                    if (camera.fov_mode != "headset" && camera.fov_mode != "game") {
                        Fail(where, fmt::format("fov.mode '{}' is not headset or game", camera.fov_mode));
                    }
                    if (!camera.fov_target.empty() && camera.fov_kind != "rad_vertical" &&
                        camera.fov_kind != "deg_vertical") {
                        Fail(where, fmt::format("fov.kind '{}' not supported yet (rad_vertical, deg_vertical)",
                                                camera.fov_kind));
                    }
                }
                camera.height_offset_m = static_cast<float>((*table)["height_offset_m"].value_or(0.0));
                camera.cut_fade_ms = OptU32(*table, "cut_fade_ms", where);
                camera.cut_field = (*table)["cut_field"].value_or(std::string{});
                camera.cut_jump_units = static_cast<float>((*table)["cut_jump_units"].value_or(0.0));
                camera.cut_jump_deg = static_cast<float>((*table)["cut_jump_deg"].value_or(0.0));
                camera.view_setting = (*table)["view_setting"].value_or(std::string{});
                camera.vignette_setting = (*table)["vignette_setting"].value_or(std::string{});
                for (const std::string* key : {&camera.view_setting, &camera.vignette_setting}) {
                    if (key->empty()) {
                        continue;
                    }
                    const bool known = std::ranges::any_of(VIZR::Settings::Table, [&](const VIZR::Settings::Spec& spec) {
                        return spec.scope == VIZR::Settings::Scope::Comfort && *key == spec.key;
                    });
                    if (!known) {
                        Fail(where, fmt::format("'{}' is not a [comfort] setting", *key));
                    }
                }
                if (camera.cut_fade_ms > 1000) {
                    Fail(where, "cut_fade_ms is at most 1000");
                }
                camera.yaw_source = (*table)["yaw_source"].value_or(std::string{});
                camera.yaw_source_forward = (*table)["yaw_source_forward"].value_or(std::string{});
                if (!camera.yaw_source_forward.empty()) {
                    AxisVector(where, camera.yaw_source_forward); // validates
                }
                if (camera.mode != "passthrough" && (camera.source.empty() || camera.target.empty())) {
                    Fail(where, "a non-passthrough camera needs source and target");
                }
                profile.cameras.push_back(std::move(camera));
            }
        }

        if (const auto* input = game["input"].as_table()) {
            profile.layout_path = (*input)["layout"].value_or(std::string{});
            profile.recenter = (*input)["recenter"].value_or(std::string{"left_stick_click"});
        }
        if (const auto* render = game["render"].as_table()) {
            profile.stereo = (*render)["stereo"].value_or(std::string{"mono"});
            if (profile.stereo != "mono" && profile.stereo != "afr" && profile.stereo != "mode_c") {
                Fail("game.toml [render]",
                     fmt::format("stereo '{}' (use \"mono\", \"afr\" or \"mode_c\")", profile.stereo));
            }
            // The HUD must be able to shrink into the comfortable middle of the view.
            if (const auto fit = (*render)["hud_scale"].value<std::string>()) {
                if (*fit != "fit") {
                    Fail("game.toml [render]", fmt::format("hud_scale '{}' (use \"fit\" or 0.3..1.0)", *fit));
                }
                profile.hud_fit = true;
            } else if (const auto scale = (*render)["hud_scale"].value<double>()) {
                if (*scale < 0.3 || *scale > 1.0) {
                    Fail("game.toml [render]", fmt::format("hud_scale {} outside 0.3..1.0", *scale));
                }
                profile.hud_scale = static_cast<float>(*scale);
            }
            profile.hud_depth_m = static_cast<float>((*render)["hud_depth_m"].value_or(1.0));
            profile.flat_state = (*render)["flat_state"].value_or(std::string{});
            profile.flat_state_hook = (*render)["flat_state_hook"].value_or(std::string{});
            if (!profile.flat_state.empty() && profile.flat_state_hook.empty()) {
                Fail("game.toml [render]", "flat_state needs flat_state_hook (the hook where its fields are readable)");
            }
            if (const auto* alive = (*render)["alive_hooks"].as_array()) {
                for (const auto& node : *alive) {
                    profile.alive_hooks.push_back(node.value_or(std::string{}));
                }
            }
            // The fit cone is a key.
            profile.hud_fit_deg = static_cast<float>((*render)["hud_fit_deg"].value_or(35.0));
            if (profile.hud_fit_deg < 20.0f || profile.hud_fit_deg > 60.0f) {
                Fail("game.toml [render]", fmt::format("hud_fit_deg {} outside 20..60", profile.hud_fit_deg));
            }
            if (const auto* layers = (*render)["flat_layers"].as_array()) {
                for (const auto& node : *layers) {
                    const auto* entry = node.as_table();
                    const auto text = entry ? (*entry)["shader_hash"].value<std::string>() : std::nullopt;
                    u64 hash = 0;
                    const char* begin = text ? text->c_str() + (text->starts_with("0x") ? 2 : 0) : nullptr;
                    if (!text || std::from_chars(begin, text->c_str() + text->size(), hash, 16).ec != std::errc{} ||
                        hash == 0) {
                        Fail("game.toml [render] flat_layers", "each entry needs shader_hash = \"0x<hex>\"");
                    }
                    profile.flat_layers.push_back(hash);
                    // An entry may keep its own scale (the helmet rim stays at 1.0 while
                    // the gauges shrink to fit).
                    const auto own = (*entry)["scale"].value<double>();
                    if (own && (*own < 0.3 || *own > 1.0)) {
                        Fail("game.toml [render] flat_layers", fmt::format("scale {} outside 0.3..1.0", *own));
                    }
                    profile.flat_scales.push_back(own ? static_cast<float>(*own) : 0.0f);
                }
            }
        }

        // Game.toml [hud] per-element settings, mapped onto the address file's element list
        // once that is read (below).
        std::vector<std::pair<std::string, VIZR::HudElement>> hud_settings;
        if (const auto* labels = game["menu_labels"].as_table()) {
            for (const auto& [key, node] : *labels) {
                if (const auto text = node.value<std::string>()) {
                    profile.menu_labels[std::string{key.str()}] = *text;
                } else {
                    Fail("game.toml [menu_labels]", fmt::format("'{}' is not a string", key.str()));
                }
            }
        }
        if (const auto* hud = game["hud"].as_table()) {
            for (const auto& [key, node] : *hud) {
                const std::string name{key.str()};
                const auto* entry = node.as_table();
                if (entry == nullptr) {
                    Fail("game.toml [hud]", fmt::format("'{}' must be a table", name));
                }
                VIZR::HudElement e;
                const std::string where = fmt::format("game.toml [hud] {}", name);
                if (const auto* offset = (*entry)["offset"].as_array()) {
                    if (offset->size() != 2 || !(*offset)[0].value<double>() || !(*offset)[1].value<double>()) {
                        Fail(where, "offset must be [x, y]");
                    }
                    e.offset = {static_cast<float>(*(*offset)[0].value<double>()),
                                static_cast<float>(*(*offset)[1].value<double>())};
                }
                e.scale = static_cast<float>((*entry)["scale"].value_or(1.0));
                e.alpha = static_cast<float>((*entry)["alpha"].value_or(1.0));
                e.depth = static_cast<float>((*entry)["depth"].value_or(0.0));
                if (e.scale < 0.2f || e.scale > 3.0f || e.alpha < 0.0f || e.alpha > 1.0f || e.depth < -20.0f ||
                    e.depth > 20.0f || std::abs(e.offset[0]) > 1.0f || std::abs(e.offset[1]) > 1.0f) {
                    Fail(where, "scale 0.2..3, alpha 0..1, depth -20..20, offset -1..1");
                }
                hud_settings.emplace_back(name, e);
            }
        }

        // Patch meaning (game.toml) + file (address file).
        const auto* address_patches = address["patches"].as_table();
        if (const auto* patches = game["patches"].as_table()) {
            for (const auto& [key, node] : *patches) {
                const auto* table = node.as_table();
                if (table == nullptr) {
                    continue;
                }
                Patch patch;
                patch.name = std::string{key.str()};
                const std::string where = fmt::format("game.toml patch '{}'", patch.name);
                patch.enabled = (*table)["enabled"].value_or(false);
                const s64 hz = OptionalInt(*table, "present_hz", where).value_or(0);
                if (hz != 0 && (hz < 30 || hz > 240)) {
                    Fail(where, fmt::format("present_hz {} outside 30..240", hz));
                }
                patch.present_hz = static_cast<u32>(hz);
                if (address_patches != nullptr) {
                    patch.pchtxt = (*address_patches)[patch.name].value_or(std::string{});
                }
                profile.patches.push_back(std::move(patch));
            }
        }

        const std::string file_build_id = Required<std::string>(address, "build_id", "address file");
        if (!build_id.starts_with(file_build_id)) {
            Fail("address file", fmt::format("build_id {} does not match the game ({})",
                                             file_build_id, build_id));
        }

        if (const auto* aim = address["aim"].as_table()) {
            const std::string where = "address file [aim]";
            Aim& a = profile.aim;
            a.present = true;
            a.ray_hook = Required<std::string>(*aim, "ray_hook", where);
            for (const auto& [key, out] : {std::pair<std::string_view, u32*>{"cursor_reg", &a.cursor_reg},
                                           std::pair<std::string_view, u32*>{"ray_xform_reg", &a.ray_xform_reg}}) {
                const auto v = OptionalInt(*aim, key, where);
                if (!v || *v < 0 || *v > 30) {
                    Fail(where, fmt::format("'{}' (x0..x30) is required", key));
                }
                *out = static_cast<u32>(*v);
            }
            a.ray_xform_off = OptionalInt(*aim, "ray_xform_off", where).value_or(0);
            if (const auto* cols = (*aim)["ray_columns"].as_array(); cols != nullptr && cols->size() == 3) {
                for (std::size_t i = 0; i < 3; ++i) {
                    a.ray_columns[i] = (*cols)[i].value_or(std::string{});
                }
            }
            a.cursor_on_plane_off = OptU64(*aim, "cursor_on_plane_off", where);
            a.cursor_hit_off = OptU64(*aim, "cursor_hit_off", where);
            a.cursor_distance_off = OptU64(*aim, "cursor_distance_off", where);
            const std::string scan_source = (*aim)["scan_source"].value_or(std::string{"head"});
            if (scan_source != "head" && scan_source != "hand") {
                Fail(where, fmt::format("[aim] scan_source '{}' must be \"head\" or \"hand\"", scan_source));
            }
            a.scan_from_head = scan_source == "head";
            a.scan_state = (*aim)["scan_state"].value_or(std::string{});
            a.reticle_hook = (*aim)["reticle_hook"].value_or(std::string{});
            a.reticle_xform_reg = static_cast<u32>(OptionalInt(*aim, "reticle_xform_reg", where).value_or(2));
            if (const auto* lrs = (*aim)["head_reticle_lrs"].as_array()) {
                for (const auto& node : *lrs) {
                    const auto [module, offset] = ParseModuleAddress(where, node.value_or(std::string{}));
                    a.reticle_lr_module = module;
                    a.head_reticle_lr_offsets.push_back(offset);
                }
            }
            if (const auto* lrs = (*aim)["reticle_lrs"].as_array()) {
                for (const auto& node : *lrs) {
                    const auto [module, offset] = ParseModuleAddress(where, node.value_or(std::string{}));
                    a.reticle_lr_module = module;
                    a.reticle_lr_offsets.push_back(offset);
                }
            }
            if (const auto skip = (*aim)["reticle_skip_lr"].value<std::string>()) {
                const auto [module, offset] = ParseModuleAddress(where, *skip);
                a.reticle_skip_lr_module = module;
                a.reticle_skip_lr_offset = offset;
            }
            if (const auto* ht = (*aim)["head_reticle_target"].as_table()) {
                const auto reg = OptionalInt(*ht, "reg", where);
                const auto off = OptionalInt(*ht, "off", where);
                if (!reg || *reg < 0 || *reg > 30 || !off || *off < 0) {
                    Fail(where, "head_reticle_target needs reg (0-30) and off");
                }
                a.head_reticle_target_reg = static_cast<s32>(*reg);
                a.head_reticle_target_off = static_cast<u64>(*off);
                a.head_reticle_target_margin = static_cast<float>((*ht)["margin"].value_or(0.0));
            }
            if (const auto* lrs = (*aim)["rotate_only_lrs"].as_array()) {
                for (const auto& node : *lrs) {
                    const auto [module, offset] = ParseModuleAddress(where, node.value_or(std::string{}));
                    a.rotate_only_lr_module = module;
                    a.rotate_only_lr_offsets.push_back(offset);
                }
            }
        }

        if (const auto* snap = address["snap"].as_table()) {
            const std::string where = "address file [snap]";
            Snap& n = profile.snap;
            n.present = true;
            n.yaw_field = Required<std::string>(*snap, "yaw_field", where);
            n.push_field = Required<std::string>(*snap, "push_field", where);
            if (const auto* skip = (*snap)["skip_fields"].as_array()) {
                for (const auto& node : *skip) {
                    n.skip_fields.push_back(node.value_or(std::string{}));
                }
            }
            n.sign = static_cast<float>((*snap)["sign"].value_or(1.0));
            if (const auto* skip = (*snap)["torso_skip_fields"].as_array()) {
                for (const auto& node : *skip) {
                    n.torso_skip_fields.push_back(node.value_or(std::string{}));
                }
            }
        }

        if (const auto* arms = address["arms"].as_table()) {
            const std::string where = "address file [arms]";
            Arms& a = profile.arms;
            a.present = true;
            a.hook = Required<std::string>(*arms, "hook", where);
            const auto required = [&](std::string_view key) {
                const auto value = OptionalInt(*arms, key, where);
                if (!value) {
                    Fail(where, fmt::format("missing '{}'", key));
                }
                return *value;
            };
            a.char_reg = static_cast<u32>(required("char_reg"));
            if (a.char_reg > 30) {
                Fail(where, "char_reg must be 0-30");
            }
            if (const auto* list = (*arms)["char_fields"].as_array()) {
                for (const auto& node : *list) {
                    a.char_fields.push_back(node.value_or(std::string{}));
                }
            }
            a.pose_off_at = static_cast<u64>(required("pose_off_at"));
            a.coords_table_at = static_cast<u64>(required("coords_table_at"));
            if (const auto* chain = (*arms)["node_set_chain"].as_array()) {
                for (const auto& node : *chain) {
                    a.node_set_chain.push_back(ElemU64(node, where));
                }
            }
            a.count_at = OptU64(*arms, "count_at", where);
            a.ids_at = static_cast<u64>(required("ids_at"));
            a.left_beam_hook = (*arms)["left_beam_hook"].value_or(std::string{});
            if (!a.left_beam_hook.empty()) {
                a.left_beam_off = static_cast<u64>(required("left_beam_off"));
                a.left_beam_local_off = static_cast<u64>(required("left_beam_local_off"));
                a.left_beam_state_off = static_cast<u64>(required("left_beam_state_off"));
                a.left_beam_state_mask = static_cast<u32>(required("left_beam_state_mask"));
                a.left_beam_state_equals = static_cast<u32>(required("left_beam_state_equals"));
                if (a.left_beam_state_mask > 0xff || (a.left_beam_state_equals & ~a.left_beam_state_mask) != 0) {
                    Fail(where, "left_beam_state_mask is a byte mask and left_beam_state_equals must lie inside it");
                }
            }
            a.body_char_index = static_cast<int>(OptionalInt(*arms, "body_char_index", where).value_or(-1));
            if (a.body_char_index >= 0 && (a.body_char_index >= static_cast<int>(a.char_fields.size()) ||
                                           a.body_char_index >= 3 || a.body_char_index == 0)) {
                Fail(where, "body_char_index must name char_fields[1] or [2] (char_fields[0] is the right arm)");
            }
            if (const auto* stand = (*arms)["body_stand"].as_array()) {
                for (const auto& node : *stand) {
                    const auto* t = node.as_table();
                    const auto* rot = t ? (*t)["rot"].as_array() : nullptr;
                    const auto index = t ? OptionalInt(*t, "node", where) : std::nullopt;
                    if (!index || rot == nullptr || rot->size() != 4) {
                        Fail(where, "body_stand entries are { node = <n>, rot = [w, x, y, z] }");
                    }
                    Arms::StandNode s{.node = static_cast<u32>(*index)};
                    for (std::size_t i = 0; i < 4; ++i) {
                        s.rot[i] = static_cast<float>((*rot)[i].value_or(0.0));
                    }
                    a.body_stand.push_back(s);
                }
                const auto* roots = (*arms)["body_root_nodes"].as_array();
                const auto* rest = (*arms)["body_root_rest"].as_array();
                const auto* chain = (*arms)["body_lean_chain"].as_array();
                if (roots == nullptr || rest == nullptr || rest->size() != 3 || chain == nullptr) {
                    Fail(where, "body_stand needs body_root_nodes, body_root_rest = [x, y, z] and body_lean_chain");
                }
                for (const auto& n : *roots) {
                    a.body_root_nodes.push_back(ElemU32(n, where));
                }
                if (a.body_root_nodes.size() > 4) {
                    Fail(where, "body_root_nodes: at most 4");
                }
                for (std::size_t i = 0; i < 3; ++i) {
                    a.body_root_rest[i] = static_cast<float>((*rest)[i].value_or(0.0));
                }
                for (const auto& n : *chain) {
                    a.body_lean_chain.push_back(ElemU32(n, where));
                }
                a.body_upright_node = OptU32(*arms, "body_upright_node", where);
                if (const auto* up = (*arms)["body_upright_chain"].as_array()) {
                    for (const auto& n : *up) {
                        a.body_upright_chain.push_back(ElemU32(n, where));
                    }
                }
                a.body_hide_to_parent = (*arms)["body_hide_to_parent"].value_or(false);
                // No Prime conventions in core: signs, order and the model's axes are the profile's.
                a.body_lean_order = static_cast<float>(Required<double>(*arms, "body_lean_order", where));
                a.body_crouch_hip_sign = static_cast<float>(Required<double>(*arms, "body_crouch_hip_sign", where));
                a.body_model_left = AxisVector(where, Required<std::string>(*arms, "body_model_left", where));
                a.body_model_up = AxisVector(where, Required<std::string>(*arms, "body_model_up", where));
                a.body_model_forward = AxisVector(where, Required<std::string>(*arms, "body_model_forward", where));
                a.body_model_scale = static_cast<float>(Required<double>(*arms, "body_model_scale", where));
                const auto* lean_nodes = (*arms)["body_lean_nodes"].as_array();
                if (lean_nodes == nullptr || lean_nodes->empty()) {
                    Fail(where, "body_lean_nodes = [<node>, ...] (top down) is required with body_stand");
                }
                for (const auto& n : *lean_nodes) {
                    a.body_lean_nodes.push_back(ElemU32(n, where));
                }
                if (const auto* collapse = (*arms)["body_collapse"].as_array()) {
                    for (const auto& node : *collapse) {
                        const auto* t = node.as_table();
                        const auto n = t ? OptionalInt(*t, "node", where) : std::nullopt;
                        const auto p = t ? OptionalInt(*t, "parent", where) : std::nullopt;
                        if (!n || !p) {
                            Fail(where, "body_collapse entries are { node = <n>, parent = <its parent node> }");
                        }
                        a.body_collapse.push_back({static_cast<u32>(*n), static_cast<u32>(*p)});
                    }
                }
                a.body_torso_height = static_cast<float>(Required<double>(*arms, "body_torso_height", where));
                a.body_step_m = static_cast<float>(Required<double>(*arms, "body_step_m", where));
                a.body_leg_length = static_cast<float>(Required<double>(*arms, "body_leg_length", where));
                a.body_crouch_max = static_cast<float>(Required<double>(*arms, "body_crouch_max", where));
                // The lean reads its node live; that is safe only when the stance rewrites it every solve.
                for (const u32 lean_node : a.body_lean_nodes) {
                    if (std::none_of(a.body_stand.begin(), a.body_stand.end(),
                                     [&](const Arms::StandNode& s) { return s.node == lean_node; })) {
                        Fail(where, "every body_lean_nodes node must be one of the body_stand nodes");
                    }
                }
                if (a.body_model_scale <= 0.0f || a.body_torso_height <= 0.0f || a.body_leg_length <= 0.0f) {
                    Fail(where, "body_model_scale, body_torso_height and body_leg_length must be > 0");
                }
            }
            if (const auto* legs = (*arms)["body_legs"].as_array()) {
                for (const auto& node : *legs) {
                    a.body_legs.push_back(ElemU32(node, where));
                }
                if (a.body_legs.size() != 6) {
                    Fail(where, "body_legs = [L hip, L knee, L ankle, R hip, R knee, R ankle]");
                }
                a.body_feet_field = Required<std::string>(*arms, "body_feet_field", where);
                a.body_air_field = (*arms)["body_air_field"].value_or(std::string{});
                const auto axis = [&](std::string_view key, std::array<float, 3>& out) {
                    if (const auto* v = (*arms)[key].as_array(); v && v->size() == 3) {
                        for (std::size_t i = 0; i < 3; ++i) {
                            out[i] = static_cast<float>((*v)[i].value_or(0.0));
                        }
                    }
                };
                axis("body_swing_axis", a.body_swing_axis);
                axis("body_side_axis", a.body_side_axis);
                axis("body_knee_axis", a.body_knee_axis);
                // Speeds and the stride are in the game's world units, so no defaults in core.
                a.body_stride = static_cast<float>(Required<double>(*arms, "body_stride", where));
                a.body_walk_speed = static_cast<float>(Required<double>(*arms, "body_walk_speed", where));
                a.body_run_speed = static_cast<float>(Required<double>(*arms, "body_run_speed", where));
                a.body_still_speed = static_cast<float>(Required<double>(*arms, "body_still_speed", where));
                a.body_feet_forward = AxisVector(where, Required<std::string>(*arms, "body_feet_forward", where));
                a.body_leg_log = (*arms)["body_leg_log"].value_or(false);
                if (a.body_stride <= 0.1f || a.body_run_speed <= a.body_walk_speed || a.body_walk_speed <= a.body_still_speed) {
                    Fail(where, "body_stride > 0.1 and body_still_speed < body_walk_speed < body_run_speed");
                }
            }
            if (const auto* cams = (*arms)["body_cameras"].as_array()) {
                for (const auto& node : *cams) {
                    a.body_cameras.push_back(node.value_or(std::string{}));
                }
            }
            if (a.body_char_index > 0 && a.body_cameras.empty()) {
                Fail(where, "body_char_index needs body_cameras = [\"<camera id>\", ...] (the first-person rules)");
            }
            if (const auto* off = (*arms)["body_off_nodes"].as_array()) {
                for (const auto& node : *off) {
                    a.body_off_nodes.push_back(ElemU32(node, where));
                }
            }
            if (const auto* hide = (*arms)["body_hide_nodes"].as_array()) {
                for (const auto& node : *hide) {
                    a.body_hide_nodes.push_back(ElemU32(node, where));
                }
            }
            a.left_flags_field = (*arms)["left_flags_field"].value_or(std::string{});
            a.flags_set = OptU32(*arms, "flags_set", where);
            a.flags_clear = OptU32(*arms, "flags_clear", where);
            a.left_xf_field = (*arms)["left_xf_field"].value_or(std::string{});
            a.left_render_hook = (*arms)["left_render_hook"].value_or(std::string{});
            if (!a.left_render_hook.empty()) {
                a.left_arm_reg = static_cast<u32>(required("left_arm_reg"));
                a.left_xf_off = static_cast<u64>(required("left_xf_off"));
                if (a.left_arm_reg > 30) {
                    Fail(where, "left_arm_reg must be 0-30");
                }
                if (const auto* w = (*arms)["left_wrist"].as_array(); w && w->size() == 3) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        a.left_wrist[k] = static_cast<float>((*w)[k].value_or(0.0));
                    }
                }
                a.left_ik = (*arms)["left_ik"].value_or(false);
                if (a.left_ik) {
                    // Every left_ik key is the profile's (no game default in core).
                    const auto need_int = [&](std::string_view key) {
                        const auto v = OptionalInt(*arms, key, where);
                        if (!v || *v < 0) {
                            Fail(where, fmt::format("left_ik needs '{}'", key));
                        }
                        return static_cast<u64>(*v);
                    };
                    const auto need_vec3 = [&](std::string_view key) {
                        const auto* m = (*arms)[key].as_array();
                        if (m == nullptr || m->size() != 3) {
                            Fail(where, fmt::format("left_ik needs '{}' = [x, y, z]", key));
                        }
                        std::array<float, 3> out{};
                        for (std::size_t k = 0; k < 3; ++k) {
                            out[k] = static_cast<float>((*m)[k].value_or(0.0));
                        }
                        return out;
                    };
                    a.left_char_off = need_int("left_char_off");
                    a.left_shoulder_node = static_cast<u32>(need_int("left_shoulder_node"));
                    a.left_elbow_node = static_cast<u32>(need_int("left_elbow_node"));
                    a.left_wrist_node = static_cast<u32>(need_int("left_wrist_node"));
                    a.left_shoulder_model = need_vec3("left_shoulder_model");
                    a.left_pose_signs = need_vec3("left_pose_signs");
                    a.left_grip_m = need_vec3("left_grip_m");
                    a.left_model_scale_off = need_int("left_model_scale_off");
                    a.left_grip_rot_deg = need_vec3("left_grip_rot_deg");
                    a.left_shoulder_anchor_m = need_vec3("left_shoulder_anchor_m");
                    a.left_reach_nudge = need_vec3("left_reach_nudge");
                    a.left_anchor_id_off = need_int("left_anchor_id_off");
                    a.left_abs_banks_off = need_int("left_abs_banks_off");
                    if (const auto* m = (*arms)["left_pose_signs"].as_array(); m && m->size() == 3) {
                        for (std::size_t k = 0; k < 3; ++k) {
                            a.left_pose_signs[k] = static_cast<float>((*m)[k].value_or(1.0));
                            if (std::abs(a.left_pose_signs[k]) != 1.0f) {
                                Fail(where, "left_pose_signs are 1 or -1");
                            }
                        }
                    }
                }
            }
            if (const auto* cols = (*arms)["left_columns"].as_array(); cols && cols->size() == 3) {
                for (std::size_t k = 0; k < 3; ++k) {
                    a.left_columns[k] = (*cols)[k].value_or(std::string{});
                    static constexpr std::array<std::string_view, 6> ok{"left", "right", "up", "down", "forward", "back"};
                    if (std::ranges::find(ok, std::string_view{a.left_columns[k]}) == ok.end()) {
                        Fail(where, fmt::format("left_columns: unknown axis '{}'", a.left_columns[k]));
                    }
                }
            }
            a.node_log = (*arms)["node_log"].value_or(true);
            a.ik = (*arms)["ik"].value_or(false);
            if (a.ik) {
                a.shoulder_node = static_cast<u32>(required("shoulder_node"));
                a.elbow_node = static_cast<u32>(required("elbow_node"));
                a.gun_node = static_cast<u32>(required("gun_node"));
                a.upper_arm_sign = (*arms)["upper_arm_sign"].value_or(1.0) < 0.0 ? -1.0f : 1.0f;
                a.ik_hook = (*arms)["ik_hook"].value_or(std::string{});
                if (!a.ik_hook.empty()) {
                    a.ik_pose_reg = static_cast<u32>(required("ik_pose_reg"));
                    a.ik_char_reg = static_cast<u32>(required("ik_char_reg"));
                }
                if (a.ik_pose_reg > 30 || a.ik_char_reg > 30) {
                    Fail(where, "ik_pose_reg / ik_char_reg must be 0-30");
                }
                if (const auto* signs = (*arms)["pose_signs"].as_array(); signs && signs->size() == 3) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        a.pose_signs[k] = (*signs)[k].value_or(1.0) < 0.0 ? -1.0f : 1.0f;
                    }
                }
                a.dirty_at = static_cast<u64>(required("dirty_at"));
                a.dirty_or = static_cast<u32>(required("dirty_or"));
                if ((*arms)["rot_order"].value_or(std::string{"wxyz"}) != "wxyz") {
                    Fail(where, "rot_order: only \"wxyz\" is supported");
                }
                if (const auto* off = (*arms)["shoulder_offset_m"].as_array(); off && off->size() == 3) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        a.shoulder_offset_m[k] = static_cast<float>((*off)[k].value_or(0.0));
                    }
                }
                a.ik_log = (*arms)["ik_log"].value_or(std::string{});
            }
            if (const auto* off = (*arms)["left_offset_m"].as_array(); off && off->size() == 3) {
                for (std::size_t k = 0; k < 3; ++k) {
                    a.left_offset_m[k] = static_cast<float>((*off)[k].value_or(0.0));
                }
            }
        }

        if (const auto* aim = address["grapple_aim"].as_table()) {
            profile.grapple_aim.candidate_writes = (*aim)["candidate_writes"].value_or(true);
            const std::string where = "address file [grapple_aim]";
            GrappleAim& g = profile.grapple_aim;
            g.present = true;
            const auto need = [&](std::string_view key) {
                const auto v = OptionalInt(*aim, key, where);
                if (!v || *v < 0) {
                    Fail(where, fmt::format("missing or negative '{}'", key));
                }
                return static_cast<u64>(*v);
            };
            const auto chain = [&](std::string_view key, std::vector<u64>& out) {
                const auto* list = (*aim)[key].as_array();
                if (list == nullptr || list->empty()) {
                    Fail(where, fmt::format("'{}' = [<offset>, ...] is required", key));
                }
                for (const auto& node : *list) {
                    out.push_back(ElemU64(node, where));
                }
            };
            const auto* sites = (*aim)["sites"].as_array();
            if (sites == nullptr || sites->empty()) {
                Fail(where, "sites = [{ hook, player_reg, gl_reg }, ...] is required");
            }
            for (const auto& node : *sites) {
                const auto* t = node.as_table();
                if (t == nullptr) {
                    Fail(where, "sites entries are { hook, player_reg, gl_reg }");
                }
                GrappleAim::Site site;
                site.hook = Required<std::string>(*t, "hook", where);
                const auto p = OptionalInt(*t, "player_reg", where);
                const auto l = OptionalInt(*t, "gl_reg", where);
                if (!p || !l || *p < 0 || *p > 30 || *l < 0 || *l > 30) {
                    Fail(where, "each site needs player_reg and gl_reg 0-30");
                }
                site.player_reg = static_cast<u32>(*p);
                site.gl_reg = static_cast<u32>(*l);
                g.sites.push_back(std::move(site));
            }
            g.candidate_off = need("candidate_off");
            g.list_off = need("list_off");
            g.list_data_off = need("list_data_off"); // The game's layout, no defaults in core
            chain("table_chain", g.table_chain);
            g.table_add = OptU64(*aim, "table_add", where);
            g.deleted_off = OptU64(*aim, "deleted_off", where);
            g.deleted_mask = OptU64(*aim, "deleted_mask", where);
            chain("point_chain", g.point_chain);
            g.stride = need("stride");
            g.id_index_mask = need("id_index_mask");
            if (g.stride == 0 || g.id_index_mask == 0) {
                Fail(where, "stride and id_index_mask must be non-zero");
            }
            g.id_check_off = need("id_check_off");
            g.flags_off = need("flags_off");
            g.flags_mask = need("flags_mask");
            g.flags_value = need("flags_value");
            g.entity_off = need("entity_off");
            const auto [module, offset] = ParseModuleAddress(where, Required<std::string>(*aim, "vtable", where));
            g.vtable_module = module;
            g.vtable_offset = offset;
            g.max_angle_deg = static_cast<float>(Required<double>(*aim, "max_angle_deg", where));
            g.left_aim_yaw_offset_deg = static_cast<float>((*aim)["left_aim_yaw_offset_deg"].value_or(0.0));
            g.scan_max_angle_deg = static_cast<float>((*aim)["scan_max_angle_deg"].value_or(0.0));
            if (g.scan_max_angle_deg > 0.0f) {
                g.scan_holder_off = need("scan_holder_off");
                g.scan_info_off = need("scan_info_off");
                g.scan_info_flag_off = need("scan_info_flag_off");
                chain("scan_lists", g.scan_lists);
                if ((*aim)["scan_state_offs"]) {
                    chain("scan_state_offs", g.scan_state_offs);
                    if (g.scan_state_offs.size() != 4) {
                        Fail(where, "scan_state_offs = [orbit state, target, scan state, scan time]");
                    }
                }
                if ((*aim)["release_log_offs"]) { // U8 player fields logged around a scan-lock release
                    chain("release_log_offs", g.release_log_offs);
                }
            }
            g.range_field = Required<std::string>(*aim, "range_field", where);
            g.invalid_field = Required<std::string>(*aim, "invalid_field", where);
            if (const auto* traces = (*aim)["zl_trace"].as_array()) {
                for (const auto& node : *traces) {
                    const auto* tt = node.as_table();
                    const std::string twhere = where + " zl_trace";
                    if (tt == nullptr) {
                        Fail(twhere, "entries are tables");
                    }
                    GrappleAim::ZlTrace z;
                    z.hook = Required<std::string>(*tt, "hook", twhere);
                    z.label = Required<std::string>(*tt, "label", twhere);
                    const auto reg = OptionalInt(*tt, "reg", twhere);
                    const auto preg = OptionalInt(*tt, "player_reg", twhere);
                    if (!reg || *reg < 0 || *reg > 30 || !preg || *preg < 0 || *preg > 30) {
                        Fail(twhere, "reg and player_reg (x0..x30) are required");
                    }
                    z.reg = static_cast<u32>(*reg);
                    z.player_reg = static_cast<u32>(*preg);
                    if (const auto* offs = (*tt)["player_offs"].as_array()) {
                        for (const auto& o : *offs) {
                            z.player_offs.push_back(ElemU64(o, twhere));
                        }
                    }
                    g.zl_trace.push_back(std::move(z));
                }
            }
            if (const auto* pl = (*aim)["press_log"].as_table()) {
                const std::string lwhere = where + " press_log";
                auto& p = g.press_log;
                p.present = true;
                if (const auto* lists = (*pl)["lists"].as_array()) {
                    for (const auto& o : *lists) {
                        p.lists.push_back(ElemU64(o, lwhere));
                    }
                }
                const auto u = [&](const char* key) {
                    const auto v = OptionalInt(*pl, key, lwhere);
                    if (!v || *v < 0) {
                        Fail(lwhere, fmt::format("{} is required", key));
                    }
                    return static_cast<u64>(*v);
                };
                p.disable_off = u("disable_off");
                p.read_list_off = u("read_list_off");
                p.read_count_off = u("read_count_off");
                p.read_data_off = u("read_data_off");
                p.read_stride = u("read_stride");
                p.read_id_off = u("read_id_off");
                p.read_flags_off = u("read_flags_off");
                if (p.read_stride == 0) {
                    Fail(lwhere, "read_stride must be > 0");
                }
            }
            if (const auto* pts = (*aim)["points"].as_table()) {
                const std::string pwhere = where + " points";
                auto& p = g.points;
                p.present = true;
                const auto pneed = [&](std::string_view key) {
                    const auto v = OptionalInt(*pts, key, pwhere);
                    if (!v || *v < 0) {
                        Fail(pwhere, fmt::format("missing or negative '{}'", key));
                    }
                    return static_cast<u64>(*v);
                };
                p.list_ptr_off = pneed("list_ptr_off");
                p.list_off = pneed("list_off");
                p.list_data_off = pneed("list_data_off");
                p.bucket_stride = pneed("bucket_stride");
                p.head_goc_off = pneed("head_goc_off");
                p.goc_next_off = pneed("goc_next_off");
                p.goc_entity_off = pneed("goc_entity_off");
                p.id_off = pneed("id_off");
                p.area_off = pneed("area_off");
                p.active_off = pneed("active_off");
                p.active_mask = pneed("active_mask");
                p.player_area_off = pneed("player_area_off");
                if (p.bucket_stride == 0 || p.active_mask == 0) {
                    Fail(pwhere, "bucket_stride and active_mask must be non-zero");
                }
                p.min_range = static_cast<float>(Required<double>(*pts, "min_range", pwhere));
            }
            if (const auto* zl = (*aim)["zl_log"].as_table()) {
                const std::string zwhere = where + " zl_log";
                auto& z = g.zl_log;
                z.present = true;
                z.hook = Required<std::string>(*zl, "hook", zwhere);
                const auto reg = OptionalInt(*zl, "candidate_reg", zwhere);
                if (!reg || *reg < 0 || *reg > 30) {
                    Fail(zwhere, "candidate_reg must be x0..x30");
                }
                z.candidate_reg = static_cast<u32>(*reg);
                if (const auto* offs = (*zl)["player_offs"].as_array()) {
                    for (const auto& o : *offs) {
                        z.player_offs.push_back(ElemU64(o, where));
                    }
                }
                if (const auto* fields = (*zl)["fields"].as_array()) {
                    for (const auto& f : *fields) {
                        z.fields.push_back(f.value_or(std::string{}));
                    }
                }
            }
        }
        if (const auto* fade = address["fade"].as_table()) {
            if (const auto* list = (*fade)["states"].as_array()) {
                for (const auto& node : *list) {
                    profile.fade_states.push_back(node.value_or(std::string{}));
                }
            }
        }

        if (const auto* sl = address["scan_log"].as_table()) {
            if (const auto* fields = (*sl)["fields"].as_array()) {
                for (const auto& node : *fields) {
                    profile.scan_log_fields.push_back(node.value_or(std::string{})); // checked after [fields]
                }
            }
        }
        if (const auto* ml = address["move_log"].as_table()) {
            if (const auto* fields = (*ml)["fields"].as_array()) {
                for (const auto& node : *fields) {
                    profile.move_log_fields.push_back(node.value_or(std::string{})); // checked after [fields]
                }
            }
        }
        if (const auto* tr = address["tick_rate"].as_table()) {
            const std::string where = "address file [tick_rate]";
            TickRate& t = profile.tick_rate;
            t.present = true;
            t.module = Required<std::string>(*tr, "module", where);
            t.match_max = OptU32(*tr, "match_max", where);
            const auto* sites = (*tr)["sites"].as_array();
            const auto* original = (*tr)["original"].as_array();
            const auto* rates = (*tr)["rates"].as_table();
            if (sites == nullptr || original == nullptr || rates == nullptr || sites->size() != original->size() ||
                sites->empty()) {
                Fail(where, "needs sites, original (one per site) and [tick_rate.rates]");
            }
            for (std::size_t i = 0; i < sites->size(); ++i) {
                const s64 site = (*sites)[i].value_or(s64{-1});
                if (site < 0) {
                    Fail(where, "sites are module offsets");
                }
                t.sites.push_back(static_cast<u64>(site));
                t.original.push_back(ParseBytes(where, (*original)[i].value_or(std::string{})));
            }
            for (const auto& [key, node] : *rates) {
                const auto hz = std::strtoul(std::string{key.str()}.c_str(), nullptr, 10);
                const auto* bytes = node.as_array();
                if (hz < 30 || hz > 240 || bytes == nullptr || bytes->size() != t.sites.size()) {
                    Fail(where, fmt::format("rates.{}: a rate in Hz with one byte string per site", key.str()));
                }
                std::vector<std::vector<u8>> per_site;
                for (std::size_t i = 0; i < bytes->size(); ++i) {
                    per_site.push_back(ParseBytes(where, (*bytes)[i].value_or(std::string{})));
                    if (per_site.back().size() != t.original[i].size()) {
                        Fail(where, fmt::format("rates.{}: site {} has {} bytes, the original {}", key.str(), i,
                                                per_site.back().size(), t.original[i].size()));
                    }
                }
                t.rates.emplace(static_cast<u32>(hz), std::move(per_site));
            }
        }
        if (const auto* oh = address["orbit_head"].as_table()) {
            const std::string where = "address file [orbit_head]";
            OrbitHead& o = profile.orbit_head;
            o.present = true;
            o.hook = Required<std::string>(*oh, "hook", where);
            const auto reg = OptionalInt(*oh, "reg", where);
            if (!reg || *reg < 0 || *reg > 30) {
                Fail(where, "reg (0-30) is required");
            }
            o.reg = static_cast<u32>(*reg);
            std::tie(o.buffer_module, o.buffer_offset) = ParseModuleAddress(where, Required<std::string>(*oh, "buffer", where));
            if (const auto facing = (*oh)["facing_buffer"].value<std::string>()) {
                std::tie(o.facing_module, o.facing_offset) = ParseModuleAddress(where, *facing);
                o.facing = true;
                if (const auto off = OptionalInt(*oh, "facing_target_off", where)) {
                    o.facing_target_off = static_cast<u64>(*off);
                }
            }
            o.lock_field = (*oh)["lock_field"].value_or(std::string{}); // checked after [fields]
            if (!o.lock_field.empty()) {
                const auto lo = OptionalInt(*oh, "lock_min", where);
                const auto hi = OptionalInt(*oh, "lock_max", where);
                if (!lo || !hi || *lo > *hi) {
                    Fail(where, "lock_field needs lock_min and lock_max (the values that mean a lock)");
                }
                o.lock_min = *lo;
                o.lock_max = *hi;
            }
        }
        if (const auto* lw = address["left_window"].as_table()) {
            const std::string where = "address file [left_window]";
            LeftWindow& w = profile.left_window;
            w.present = true;
            for (const auto* key : {"open_hooks", "close_hooks"}) {
                auto& out = std::string_view{key} == "open_hooks" ? w.open_hooks : w.close_hooks;
                if (const auto* list = (*lw)[key].as_array()) {
                    for (const auto& o : *list) {
                        const auto name = o.value<std::string>();
                        if (!name || name->empty()) {
                            Fail(where, std::string{key} + " must list hook names");
                        }
                        out.push_back(*name);
                    }
                }
            }
            if (w.close_hooks.empty()) {
                Fail(where, "close_hooks is required");
            }
            w.prerender_hook = (*lw)["prerender_hook"].value_or(std::string{});
            if (!w.prerender_hook.empty()) {
                const auto off = OptionalInt(*lw, "prerender_flags_off", where);
                const auto mask = OptionalInt(*lw, "prerender_flags_mask", where);
                const auto equals = OptionalInt(*lw, "prerender_flags_equals", where);
                if (!off || !mask || !equals || *off < 0) {
                    Fail(where, "prerender_hook needs prerender_flags_off, prerender_flags_mask and prerender_flags_equals");
                }
                w.prerender_flags_off = static_cast<u64>(*off);
                w.prerender_flags_mask = static_cast<u32>(*mask);
                w.prerender_flags_equals = static_cast<u32>(*equals);
            }
            if (const auto state = OptionalInt(*lw, "state_off", where); state && *state > 0) {
                w.state_off = static_cast<u64>(*state);
                w.shown_state = static_cast<u32>(OptionalInt(*lw, "shown_state", where).value_or(9));
                if (const auto* offs = (*lw)["player_offs"].as_array()) {
                    for (const auto& o : *offs) {
                        w.player_offs.push_back(ElemU64(o, where));
                    }
                }
                w.owned_field = (*lw)["owned_field"].value_or(std::string{});
                if (const auto* ids = (*lw)["device_rel_ids"].as_array()) {
                    for (const auto& o : *ids) {
                        w.device_rel_ids.push_back(static_cast<u32>(ElemU64(o, where)));
                    }
                }
                w.device_compose_hook = (*lw)["device_compose_hook"].value_or(std::string{});
                w.device_compose_pose_reg = static_cast<u32>((*lw)["device_compose_pose_reg"].value_or(int64_t{19}));
                w.device_elbow_id = static_cast<u32>((*lw)["device_elbow_id"].value_or(int64_t{0x300}));
                if (const auto* ids = (*lw)["device_part_rel_ids"].as_array()) {
                    for (const auto& o : *ids) {
                        w.device_part_rel_ids.push_back(static_cast<u32>(ElemU64(o, where)));
                    }
                }
                if (const auto* ids = (*lw)["device_part_bind_ids"].as_array()) {
                    for (const auto& o : *ids) {
                        w.device_part_bind_ids.push_back(static_cast<u32>(ElemU64(o, where)));
                    }
                }
                w.device_nodeid_hook = (*lw)["device_nodeid_hook"].value_or(std::string{});
                w.device_write = (*lw)["device_write"].value_or(false);
                if (w.device_write) {
                    const auto need = [&](const char* key) {
                        const auto v = OptionalInt(*lw, key, where);
                        if (!v || *v < 0) {
                            Fail(where, std::string{"device_write needs "} + key);
                        }
                        return static_cast<u64>(*v);
                    };
                    w.device_skip_player_off = need("device_skip_player_off");
                    w.device_abs_banks_off = need("device_abs_banks_off");
                    w.device_parent_slot = need("device_parent_slot");
                    if (const auto* slots = (*lw)["device_abs_slots"].as_array()) {
                        for (const auto& o : *slots) {
                            w.device_abs_slots.push_back(ElemU64(o, where));
                        }
                    }
                    if (const auto* locals = (*lw)["device_fallback_local"].as_array()) {
                        for (const auto& row : *locals) {
                            const auto* m = row.as_array();
                            if (m == nullptr || m->size() != 12) {
                                Fail(where, "device_fallback_local: each entry is 12 numbers (3x4 row-major)");
                            }
                            std::array<float, 12> x{};
                            for (std::size_t i = 0; i < 12; ++i) {
                                x[i] = static_cast<float>((*m)[i].value_or(0.0));
                            }
                            w.device_fallback_local.push_back(x);
                        }
                    }
                }
                if (const auto* words = (*lw)["arm_words"].as_array()) {
                    for (const auto& o : *words) {
                        w.arm_words.push_back(ElemU64(o, where));
                    }
                }
                if (const auto* locals = (*lw)["device_locals"].as_array()) {
                    for (const auto& o : *locals) {
                        w.device_locals.push_back(ElemU64(o, where));
                    }
                }
            }
            w.ik_window_only = (*lw)["ik_window_only"].value_or(false);
            w.probe = (*lw)["probe"].value_or(false);
        }
        if (const auto* cn = address["camera_now"].as_table()) {
            const std::string where = "address file [camera_now]";
            CameraNow& c = profile.camera_now;
            c.present = true;
            const auto reg = OptionalInt(*cn, "reg", where);
            if (!reg || *reg < 0 || *reg > 30) {
                Fail(where, "reg (0-30) is required");
            }
            c.reg = static_cast<u32>(*reg);
            const auto camera_off = OptionalInt(*cn, "camera_off", where);
            if (!camera_off || *camera_off < 0) {
                Fail(where, "camera_off is required");
            }
            c.camera_off = static_cast<u64>(*camera_off);
            if (const auto* skips = (*cn)["skip_counts"].as_array()) {
                for (const auto& o : *skips) {
                    c.skip_counts.push_back(ElemU64(o, where));
                }
            }
            c.shake_off = OptionalInt(*cn, "shake_off", where).value_or(-1);
            c.lock_state_off = OptionalInt(*cn, "lock_state_off", where).value_or(-1);
            if (const auto* chain = (*cn)["chain"].as_array()) {
                for (const auto& o : *chain) {
                    c.chain.push_back(ElemU64(o, where));
                }
            }
            if (c.chain.empty()) {
                Fail(where, "chain: at least one step");
            }
            c.max_error = static_cast<float>((*cn)["max_error"].value_or(0.01));
        }
        if (const auto* lr = address["lock_ring"].as_table()) {
            const std::string where = "address file [lock_ring]";
            LockRing& l = profile.lock_ring;
            l.present = true;
            if (const auto* lrs = (*lr)["lrs"].as_array()) {
                for (const auto& node : *lrs) {
                    const auto [module, offset] = ParseModuleAddress(where, node.value_or(std::string{}));
                    l.lr_module = module;
                    l.lr_offsets.push_back(offset);
                }
            }
            if (l.lr_offsets.empty()) {
                Fail(where, "lrs: at least one caller");
            }
            const auto target = OptionalInt(*lr, "target_off", where);
            if (!target || *target < 0) {
                Fail(where, "target_off is required");
            }
            l.target_off = static_cast<u64>(*target);
            l.distance = static_cast<float>(Required<double>(*lr, "distance", where));
            if (!(l.distance > 0.0f)) {
                Fail(where, "distance must be > 0");
            }
            if (const auto* op = (*lr)["orbit_point"].as_table()) {
                const auto reg = OptionalInt(*op, "reg", where);
                const auto off = OptionalInt(*op, "off", where);
                if (!reg || *reg < 0 || *reg > 30 || !off || *off < 0) {
                    Fail(where, "orbit_point needs reg (0-30) and off");
                }
                l.orbit_point_reg = static_cast<s32>(*reg);
                l.orbit_point_off = static_cast<u64>(*off);
            }
            l.surface_margin = static_cast<float>((*lr)["surface_margin"].value_or(0.0));
        }
        if (const auto* dial = address["visor_dial"].as_table()) {
            const std::string where = "address file [visor_dial]";
            VisorDial& d = profile.visor_dial;
            d.present = true;
            d.hook = Required<std::string>(*dial, "hook", where);
            const auto required = [&](std::string_view key) {
                const auto value = OptionalInt(*dial, key, where);
                if (!value) {
                    Fail(where, fmt::format("missing '{}'", key));
                }
                return *value;
            };
            if (const auto* regs = (*dial)["choice_regs"].as_array()) {
                for (const auto& node : *regs) {
                    const s64 r = node.value_or(s64{-1});
                    if (r < 0 || r > 30) {
                        Fail(where, "choice_regs entries must be 0-30");
                    }
                    d.choice_regs.push_back(static_cast<u32>(r));
                }
                d.float_reg = OptU32(*dial, "float_reg", where);
                d.float_value = static_cast<float>((*dial)["float_value"].value_or(0.5));
                d.float_alt = static_cast<float>((*dial)["float_alt"].value_or(0.0));
                d.override_frames = OptU32(*dial, "override_frames", where);
                d.update_hook = (*dial)["update_hook"].value_or(std::string{});
                if (!d.update_hook.empty()) {
                    d.menu_reg = static_cast<u32>(required("menu_reg"));
                    d.menu_off = static_cast<u64>(required("menu_off"));
                    d.choice_off = static_cast<u64>(required("choice_off"));
                    d.fade_off = static_cast<u64>(required("fade_off"));
                    d.fade_src_off = static_cast<u64>(required("fade_src_off"));
                    d.flags_off = OptU64(*dial, "flags_off", where);
                    d.flags_or = OptU32(*dial, "flags_or", where);
                    if (d.menu_reg > 30) {
                        Fail(where, "menu_reg must be 0-30");
                    }
                }
                if (d.float_value >= 1.0f || d.float_value - d.float_alt < 0.0f) {
                    Fail(where, "float_value must stay in [0, 1) (1.0 or more drops the title)");
                }
            }
            if (d.choice_regs.empty()) {
                d.menu_reg = static_cast<u32>(required("menu_reg"));
                d.menu_off = static_cast<u64>(required("menu_off"));
                d.choice_off = static_cast<u64>(required("choice_off"));
            }
            d.turn_off = OptU64(*dial, "turn_off", where);
            d.turn = static_cast<float>((*dial)["turn"].value_or(0.5));
            d.state_off = OptU64(*dial, "state_off", where);
            d.state = OptU32(*dial, "state", where);
            d.flags_off = OptU64(*dial, "flags_off", where);
            d.flags_or = OptU32(*dial, "flags_or", where);
            if (d.menu_reg > 30) {
                Fail(where, "menu_reg must be 0-30");
            }
        }

        if (const auto* hands = address["hands"].as_table()) {
            const std::string where = "address file [hands]";
            Hands& h = profile.hands;
            h.present = true;
            h.hook = Required<std::string>(*hands, "hook", where);
            const auto* write = (*hands)["write"].as_array();
            if (write == nullptr || write->empty()) {
                Fail(where, "write = [\"<mat3x4 field>\", ...] is required");
            }
            for (const auto& node : *write) {
                h.write.push_back(node.value_or(std::string{}));
            }
            if (const auto* offset = (*hands)["offset_m"].as_array(); offset != nullptr && offset->size() == 3) {
                for (std::size_t i = 0; i < 3; ++i) {
                    h.offset_m[i] = static_cast<float>((*offset)[i].value_or(0.0));
                }
            }
            h.fire_hook = (*hands)["fire_hook"].value_or(std::string{});
            h.fire_field = (*hands)["fire_field"].value_or(std::string{});
            h.fire_origin_forward = static_cast<float>((*hands)["fire_origin_forward"].value_or(0.0));
        }

        if (const auto* states = address["states"].as_table()) {
            for (const auto& [key, node] : *states) {
                const auto* t = node.as_table();
                if (t == nullptr || profile.states.size() >= 32) {
                    Fail("address file [states]", fmt::format("'{}' must be a when-table (at most 32 states)", key.str()));
                }
                const std::string where = fmt::format("address file state '{}'", key.str());
                if ((*t)["kind"].value_or(std::string{}) == "entity_vtable") {
                    GameProfile::EntityState e;
                    e.id_field = Required<std::string>(*t, "id_field", where);
                    e.table_field = Required<std::string>(*t, "table_field", where);
                    e.stride = OptU64(*t, "stride", where);
                    e.id_index_mask = OptU64(*t, "id_index_mask", where);
                    if (e.stride == 0 || e.id_index_mask == 0) {
                        Fail(where, "an entity_vtable state needs stride and id_index_mask");
                    }
                    e.id_check_off = OptU64(*t, "id_check_off", where);
                    e.flags_off = OptU64(*t, "flags_off", where);
                    e.flags_mask = OptU64(*t, "flags_mask", where);
                    e.flags_value = OptU64(*t, "flags_value", where);
                    e.entity_off = OptU64(*t, "entity_off", where);
                    const auto [module, offset] = ParseModuleAddress(where, Required<std::string>(*t, "vtable", where));
                    e.vtable_module = module;
                    e.vtable_offset = offset;
                    profile.states.emplace_back(std::string{key.str()}, When{});
                    profile.state_entities.push_back(e);
                    continue;
                }
                profile.states.emplace_back(std::string{key.str()}, ParseWhen(where, *t));
                profile.state_entities.push_back(std::nullopt);
            }
        }

        if (const auto* widgets = address["hud_widgets"].as_table()) {
            const std::string where = "address file [hud_widgets]";
            HudWidgets& w = profile.hud_widgets;
            w.present = true;
            w.enter_hook = Required<std::string>(*widgets, "enter_hook", where);
            w.exit_hook = Required<std::string>(*widgets, "exit_hook", where);
            const auto required = [&](std::string_view key) {
                const auto v = OptionalInt(*widgets, key, where);
                if (!v) {
                    Fail(where, fmt::format("missing key '{}'", key));
                }
                return static_cast<u64>(*v);
            };
            w.world_xf_offset = required("world_xf_offset");
            w.first_child_offset = required("first_child_offset");
            w.next_sibling_offset = required("next_sibling_offset");
            w.alpha_offset = required("alpha_offset");
            const auto byte_test = [&](const toml::node& n) {
                const auto* pair = n.as_array();
                if (pair == nullptr || pair->size() != 2 || !(*pair)[0].is_integer() || !(*pair)[1].is_integer()) {
                    Fail(where, "drawn_flags / alpha_bypass entries are [offset, mask]");
                }
                return std::pair<u64, u8>{ElemU64((*pair)[0], where),
                                          static_cast<u8>(ElemUnsigned((*pair)[1], where, 0xff))};
            };
            if (const auto* flags = (*widgets)["drawn_flags"].as_array()) {
                for (const auto& n : *flags) {
                    w.drawn_flags.push_back(byte_test(n));
                }
            }
            if (const auto* bypass = (*widgets)["alpha_bypass"].node()) {
                w.alpha_bypass = byte_test(*bypass);
            }
            w.units_per_screen_height = static_cast<float>(Required<double>(*widgets, "units_per_screen_height", where));
            if (const auto* cam = (*widgets)["extent_camera"].as_array()) {
                if (cam->size() != 3) {
                    Fail(where, "extent_camera = [vertical fov deg, distance to the HUD plane, aspect]");
                }
                w.extent_camera = std::array<float, 3>{static_cast<float>((*cam)[0].value_or(0.0)),
                                                       static_cast<float>((*cam)[1].value_or(0.0)),
                                                       static_cast<float>((*cam)[2].value_or(0.0))};
            }
            w.state_field = (*widgets)["state_field"].value_or(std::string{});
            const auto* elements = (*widgets)["elements"].as_array();
            if (elements == nullptr || elements->empty() || elements->size() > VIZR::HudMaxElements) {
                Fail(where, fmt::format("elements = [{{ name, root_field, only_in_state }}, ...] (1..{})",
                                        VIZR::HudMaxElements));
            }
            for (const auto& node : *elements) {
                const auto* t = node.as_table();
                if (t == nullptr) {
                    Fail(where, "each element is a table");
                }
                HudWidgets::Element element;
                element.name = Required<std::string>(*t, "name", where);
                element.kind = (*t)["kind"].value_or(std::string{"widget"});
                if (element.kind == "corners") {
                    element.corners_enter_hook = Required<std::string>(*t, "enter_hook", where);
                    element.corners_exit_hook = Required<std::string>(*t, "exit_hook", where);
                    const auto need = [&](std::string_view key) {
                        const auto v = OptionalInt(*t, key, where);
                        if (!v) {
                            Fail(where, fmt::format("corners element '{}' needs '{}'", element.name, key));
                        }
                        return *v;
                    };
                    element.corners_reg = static_cast<u32>(need("reg"));
                    element.count_off = static_cast<u64>(need("count_off"));
                    element.data_off = static_cast<u64>(need("data_off"));
                    element.engaged_off = static_cast<u64>(need("engaged_off"));
                    element.stride = OptU64(*t, "stride", where);
                    if (element.stride < 12) {
                        Fail(where, fmt::format("corners element '{}' needs stride (>= 12: x, y, z floats)", element.name));
                    }
                    if (element.corners_reg > 30 || element.stride < 12) {
                        Fail(where, "corners: reg 0-30, stride >= 12");
                    }
                } else if (element.kind != "widget") {
                    Fail(where, fmt::format("element '{}': kind must be \"widget\" or \"corners\"", element.name));
                } else {
                    element.root_field = Required<std::string>(*t, "root_field", where);
                }
                if (const auto* base = (*t)["base_offset"].as_array(); base && base->size() == 2) {
                    element.base_offset = {static_cast<float>((*base)[0].value_or(0.0)),
                                           static_cast<float>((*base)[1].value_or(0.0))};
                }
                element.base_scale = static_cast<float>((*t)["base_scale"].value_or(1.0));
                if (const auto state = OptionalInt(*t, "only_in_state", where)) {
                    element.only_in_state = static_cast<u32>(*state);
                }
                if (const auto* by_state = (*t)["root_field_in_state"].as_array()) {
                    for (const auto& entry : *by_state) {
                        const auto* pair = entry.as_array();
                        const auto st = pair && pair->size() == 2 ? (*pair)[0].value<int64_t>() : std::nullopt;
                        const auto field = pair && pair->size() == 2 ? (*pair)[1].value<std::string>() : std::nullopt;
                        if (!st || *st < 0 || !field || field->empty()) {
                            Fail(where, "root_field_in_state = [[state, \"field\"], ...]");
                        }
                        element.root_field_in_state.emplace_back(static_cast<u32>(*st), *field);
                    }
                }
                if (element.name.empty() || element.name.size() >= 24) {
                    Fail(where, fmt::format("element name '{}' must be 1..23 characters", element.name));
                }
                w.elements.push_back(std::move(element));
            }
            if (const auto* roots = (*widgets)["whole_roots"].as_array()) {
                for (const auto& node : *roots) {
                    const auto name = node.value<std::string>();
                    if (!name) {
                        Fail(where, "whole_roots entries are field names");
                    }
                    w.whole_roots.push_back(*name);
                }
            }
            profile.hud.count = static_cast<u32>(w.elements.size());
            for (std::size_t i = 0; i < w.elements.size(); ++i) {
                std::ranges::copy(w.elements[i].name, profile.hud.names[i].begin());
            }
        }
        for (const auto& [name, setting] : hud_settings) {
            const auto& list = profile.hud_widgets.elements;
            const auto it = std::ranges::find(list, name, &HudWidgets::Element::name);
            if (it == list.end()) {
                std::string known;
                for (const auto& element : list) {
                    known += (known.empty() ? "" : ", ") + element.name;
                }
                Fail("game.toml [hud]", fmt::format("unknown element '{}' (this game: {})", name,
                                                    known.empty() ? std::string{"no [hud_widgets]"} : known));
            }
            profile.hud.elements[static_cast<std::size_t>(it - list.begin())] = setting;
        }

        if (const auto* measure = address["arm_measure"].as_table()) {
            const std::string where = "address file [arm_measure]";
            auto& m = profile.arm_measure;
            m.present = true;
            m.compose_hook = Required<std::string>(*measure, "compose_hook", where);
            const auto reg = OptionalInt(*measure, "pose_reg", where);
            const auto* ids = (*measure)["abs_ids"].as_array();
            if (!reg || *reg < 0 || *reg > 30 || ids == nullptr || ids->size() != 3) {
                Fail(where, "pose_reg (x0..x30) and abs_ids = [shoulder, elbow, wrist] are required");
            }
            m.pose_reg = static_cast<u32>(*reg);
            for (std::size_t k = 0; k < 3; ++k) {
                m.abs_ids[k] = ElemU32((*ids)[k], where);
            }
            m.render_hook = Required<std::string>(*measure, "render_hook", where);
            const auto need = [&](std::string_view key) {
                const auto v = OptionalInt(*measure, key, where);
                if (!v || *v < 0) {
                    Fail(where, fmt::format("missing '{}'", key));
                }
                return static_cast<u64>(*v);
            };
            m.render_data_reg = static_cast<u32>(need("render_data_reg"));
            m.char_reg = static_cast<u32>(need("char_reg"));
            m.skin_ptr_off = need("skin_ptr_off");
            m.draw_hook = Required<std::string>(*measure, "draw_hook", where);
            m.draw_model_reg = static_cast<u32>(need("draw_model_reg"));
            m.draw_xf_sp_off = need("draw_xf_sp_off");
            m.model_data_off = need("model_data_off");
            const auto vec3 = [&](std::string_view key, std::array<float, 3>& out) {
                const auto* v = (*measure)[key].as_array();
                if (v == nullptr || v->size() != 3) {
                    Fail(where, std::string{key} + " = [x, y, z] is required");
                }
                for (std::size_t j = 0; j < 3; ++j) {
                    out[j] = static_cast<float>((*v)[j].value_or(0.0));
                }
            };
            m.flags_off = need("flags_off");
            m.list_rel_off = need("list_rel_off");
            m.list_add = need("list_add");
            vec3("hand_forward_rest", m.hand_forward_rest);
            vec3("hand_back_rest", m.hand_back_rest);
            m.skin_stride = need("skin_stride");
            const auto* rest = (*measure)["rest"].as_array();
            if (rest == nullptr || rest->size() != 3) {
                Fail(where, "rest = [[shoulder], [elbow], [wrist]] (bind-pose joint positions) is required");
            }
            for (std::size_t k = 0; k < 3; ++k) {
                const auto* v = (*rest)[k].as_array();
                if (v == nullptr || v->size() != 3) {
                    Fail(where, "rest entries are [x, y, z]");
                }
                for (std::size_t j = 0; j < 3; ++j) {
                    m.rest[k][j] = static_cast<float>((*v)[j].value_or(0.0));
                }
            }
        }
        if (const auto* hint = address["hint_camera"].as_table()) {
            const std::string where = "address file [hint_camera]";
            auto& h = profile.hint_camera;
            h.present = true;
            h.hook = Required<std::string>(*hint, "hook", where);
            const auto reg = OptionalInt(*hint, "camera_reg", where);
            if (!reg || *reg < 0 || *reg > 30) {
                Fail(where, "camera_reg must be x0..x30");
            }
            h.camera_reg = static_cast<u32>(*reg);
            h.camera_field = Required<std::string>(*hint, "camera_field", where);
            if (const auto* when = (*hint)["when"].as_table()) {
                h.when = ParseWhen(where, *when);
            }
        }
        if (const auto* lens = address["scan_lens"].as_table()) {
            const std::string where = "address file [scan_lens]";
            ScanLens& l = profile.scan_lens;
            l.present = true;
            const auto* hooks = (*lens)["hooks"].as_array();
            const auto* callers = (*lens)["callers"].as_array();
            if (hooks == nullptr || hooks->empty() || callers == nullptr || callers->empty()) {
                Fail(where, "hooks = [\"<hook>\", ...] and callers = [<offset>, ...] are required");
            }
            for (const auto& node : *hooks) {
                l.hooks.push_back(node.value_or(std::string{}));
            }
            for (const auto& node : *callers) {
                l.callers.push_back(ElemU64(node, where));
            }
            l.module = Required<std::string>(*lens, "module", where);
            const auto reg = OptionalInt(*lens, "xform_reg", where);
            if (!reg) {
                Fail(where, "missing 'xform_reg'");
            }
            l.xform_reg = static_cast<u32>(*reg);
            l.distance = static_cast<float>(Required<double>(*lens, "distance", where));
            if (l.callers.size() > 4) {
                Fail(where, "at most 4 callers (the hit counters have 4 buckets)");
            }
            if (l.distance <= 0.0f) {
                Fail(where, "distance must be > 0");
            }
            if (l.xform_reg > 30) {
                Fail(where, "xform_reg must be x0..x30");
            }
        }
        if (const auto* gui = address["gui_eye"].as_table()) {
            const std::string where = "address file [gui_eye]";
            GuiEye& g = profile.gui_eye;
            g.present = true;
            g.proj_hook = Required<std::string>(*gui, "proj_hook", where);
            g.view_hook = Required<std::string>(*gui, "view_hook", where);
            g.helmet_hook = (*gui)["helmet_hook"].value_or(std::string{});
            // Every key is the profile's (no game default in core).
            const auto need_int = [&](std::string_view key) {
                const auto v = OptionalInt(*gui, key, where);
                if (!v || *v < 0) {
                    Fail(where, fmt::format("missing '{}'", key));
                }
                return static_cast<u64>(*v);
            };
            g.proj_reg = static_cast<u32>(need_int("proj_reg"));
            g.proj_off = need_int("proj_off");
            g.view_off = need_int("view_off");
            if (!g.helmet_hook.empty()) {
                g.helmet_proj_off = need_int("helmet_proj_off");
                g.helmet_view_off = need_int("helmet_view_off");
            }
            g.units_per_m = static_cast<float>(Required<double>(*gui, "units_per_m", where));
            g.aspect = static_cast<float>(Required<double>(*gui, "aspect", where));
            g.eye_pass = static_cast<u32>(need_int("eye_pass"));
            g.eye_sign = static_cast<float>(Required<double>(*gui, "eye_sign", where));
            g.projection = Required<std::string>(*gui, "projection", where);
            if (g.projection != "world" && g.projection != "game") {
                Fail(where, "projection is \"world\" or \"game\"");
            }
            if (g.proj_reg > 30 || g.units_per_m <= 0.0f || g.aspect <= 0.0f || std::abs(g.eye_sign) != 1.0f) {
                Fail(where, "proj_reg must be x0..x30, units_per_m and aspect > 0, eye_sign 1 or -1");
            }
        }
        if (const auto* modec = address["mode_c"].as_table()) {
            const std::string where = "address file [mode_c]";
            ModeC& m = profile.mode_c;
            m.present = true;
            m.hook = Required<std::string>(*modec, "hook", where);
            m.backend = (*modec)["backend"].value_or(std::string{"render_job"});
            if (m.backend != "render_job" && m.backend != "look_at_copy") {
                Fail(where, fmt::format("backend '{}' (use render_job or look_at_copy)", m.backend));
            }
            if (m.backend == "look_at_copy") {
                m.hook_right = Required<std::string>(*modec, "hook_right", where);
                const auto required = [&](std::string_view key) {
                    const auto v = OptionalInt(*modec, key, where);
                    if (!v || *v < 0) {
                        Fail(where, fmt::format("backend look_at_copy needs '{}'", key));
                    }
                    return static_cast<u64>(*v);
                };
                m.camera_reg = static_cast<u32>(required("camera_reg"));
                if (m.camera_reg > 30) {
                    Fail(where, "camera_reg must be x0..x30");
                }
                m.cam_matrix_off = required("cam_matrix_off");
                m.cam_pos_off = required("cam_pos_off");
                m.cam_at_off = required("cam_at_off");
                m.cam_up_off = required("cam_up_off");
                if (const auto* zero = (*modec)["zero_fields"].as_array()) {
                    for (const auto& item : *zero) {
                        const auto name = item.value<std::string>();
                        if (!name) {
                            Fail(where, "zero_fields entries must be field names");
                        }
                        m.zero_fields.push_back(*name);
                    }
                }
            }
            const auto reg = [&](std::string_view key) {
                const auto v = OptionalInt(*modec, key, where);
                if (!v || *v < 0 || *v > 30) {
                    Fail(where, fmt::format("'{}' (x0..x30) is required", key));
                }
                return static_cast<u32>(*v);
            };
            m.job_reg = reg("job_reg");
            m.scene_reg = reg("scene_reg");
            if (m.job_reg > 30 || m.scene_reg > 30) {
                Fail(where, "job_reg / scene_reg must be x0..x30");
            }
            const auto required_int = [&](std::string_view key) {
                const auto v = OptionalInt(*modec, key, where);
                if (!v) {
                    Fail(where, fmt::format("missing key '{}'", key));
                }
                return static_cast<u64>(*v);
            };
            if (m.backend == "render_job") {
                m.pass_offset = required_int("pass_offset");
                m.marker_bits = static_cast<u32>(required_int("marker_bits"));
                m.draw_camera_offset = required_int("draw_camera_offset");
                m.node_transform_offset = required_int("node_transform_offset");
                const auto* passes = (*modec)["eye_passes"].as_array();
                const auto* eyes = (*modec)["eye_offsets"].as_array();
                if (passes == nullptr || passes->empty() || eyes == nullptr || eyes->size() != 2) {
                    Fail(where, "eye_passes = [<int>...] and eye_offsets = [<left>, <right>] are required");
                }
                for (const auto& item : *passes) {
                    const auto v = item.value<s64>();
                    if (!v) {
                        Fail(where, "eye_passes entries must be integers");
                    }
                    m.eye_passes.push_back(static_cast<u32>(*v));
                }
                for (std::size_t i = 0; i < 2; ++i) {
                    const auto v = (*eyes)[i].value<s64>();
                    if (!v) {
                        Fail(where, "eye_offsets entries must be integers");
                    }
                    m.eye_offsets[i] = static_cast<u64>(*v);
                }
            }
            const auto colour = [&](std::string_view key, std::array<float, 4>& out) {
                const auto* list = (*modec)[key].as_array();
                if (list == nullptr || list->size() != 4) {
                    Fail(where, fmt::format("'{}' must be 4 numbers (r, g, b, a)", key));
                }
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto v = (*list)[i].value<double>();
                    if (!v) {
                        Fail(where, fmt::format("'{}' entries must be numbers", key));
                    }
                    out[i] = static_cast<float>(*v);
                }
            };
            colour("eye_marker_left", m.marker_left);
            colour("eye_marker_right", m.marker_right);
            if (m.marker_left == m.marker_right) {
                Fail(where, "the two eye markers must differ");
            }
        }

        if (const auto* hooks = address["hooks"].as_table()) {
            for (const auto& [key, node] : *hooks) {
                const auto* table = node.as_table();
                if (table == nullptr) {
                    Fail("address file [hooks]", "entries must be tables");
                }
                Hook hook;
                hook.name = std::string{key.str()};
                const std::string where = fmt::format("address file hook '{}'", hook.name);
                hook.module = Required<std::string>(*table, "module", where);
                hook.sym = (*table)["sym"].value_or(std::string{});
                hook.sym_off = OptU64(*table, "sym_off", where);
                if (const auto offset = OptionalInt(*table, "offset", where)) {
                    hook.offset = static_cast<u64>(*offset);
                }
                hook.expect_from = OptionalInt(*table, "expect_from", where).value_or(0);
                hook.expect = ParseBytes(where, Required<std::string>(*table, "expect", where));
                hook.enabled = (*table)["enabled"].value_or(true);
                if (hook.sym.empty() && !hook.offset) {
                    Fail(where, "needs sym or offset");
                }
                if (hook.expect.empty()) {
                    Fail(where, "expect must not be empty (wrong builds must fail loudly)");
                }
                profile.hooks.push_back(std::move(hook));
            }
        }

        if (const auto* fields = address["fields"].as_table()) {
            for (const auto& [key, node] : *fields) {
                const auto* table = node.as_table();
                if (table == nullptr) {
                    Fail("address file [fields]", "entries must be tables");
                }
                Field field;
                field.name = std::string{key.str()};
                const std::string where = fmt::format("address file field '{}'", field.name);
                field.hook = (*table)["hook"].value_or(std::string{});
                ParseBase(where, Required<std::string>(*table, "base", where), field);
                field.off = OptionalInt(*table, "off", where).value_or(0);
                field.type = ParseType(where, Required<std::string>(*table, "type", where));
                if ((field.root == RootKind::Reg || field.root == RootKind::Sp) && field.hook.empty()) {
                    Fail(where, "a register base needs a hook");
                }
                profile.fields.push_back(std::move(field));
            }
        }
        // After the whole [left_window] is read (device_write and device_locals come later in it).
        // (With device_write off the compose keys are inert: a staging may turn the write off and keep them.)
        if (const auto& w = profile.left_window; !w.device_compose_hook.empty() && w.device_write &&
                                                (w.device_part_rel_ids.empty() ||
                                                 w.device_part_rel_ids.size() > w.device_locals.size() ||
                                                 w.device_part_bind_ids.size() != w.device_part_rel_ids.size())) {
            Fail("address file [left_window]",
                 "device_compose_hook needs one device_part_rel_ids and device_part_bind_ids entry per "
                 "written part");
        }
        if (const auto& w = profile.left_window; w.device_write) {
            const std::string where = "address file [left_window]";
            if (w.device_abs_slots.empty() || w.device_abs_slots.size() > w.device_locals.size()) {
                Fail(where, "device_abs_slots needs one entry per written part (at most one per device_locals)");
            }
            for (std::size_t k = 0; k < w.device_abs_slots.size(); ++k) {
                if (w.device_abs_slots[k] == 0 && k >= w.device_fallback_local.size()) {
                    Fail(where, fmt::format("device_abs_slots[{}] = 0 needs device_fallback_local[{}]", k, k));
                }
            }
        }
        for (const auto& name : profile.scan_log_fields) {
            if (!profile.FindField(name)) {
                Fail("address file [scan_log]", fmt::format("'{}' is not a field", name));
            }
        }
        for (const auto& name : profile.move_log_fields) {
            if (!profile.FindField(name)) {
                Fail("address file [move_log]", fmt::format("'{}' is not a field", name));
            }
        }
        // [orbit_head] lock_field names a field: checked after [fields].
        if (profile.orbit_head.present && !profile.orbit_head.lock_field.empty() &&
            !profile.FindField(profile.orbit_head.lock_field)) {
            Fail("address file [orbit_head]", fmt::format("lock_field '{}' is not a field", profile.orbit_head.lock_field));
        }
        // [session_grants] names fields: read after [fields].
        if (const auto* grants = address["session_grants"].as_table()) {
            if (const auto* fields = (*grants)["fields"].as_array()) {
                for (const auto& node : *fields) {
                    const auto name = node.value<std::string>();
                    if (!name || !profile.FindField(*name)) {
                        Fail("address file [session_grants]", "fields lists a name that is not a field");
                    }
                    profile.session_grants.push_back(*name);
                }
            }
        }
        // Format v0.2: { state = "<name>" } conditions name an address-file [states] entry.
        const auto resolve = [&](auto&& self, When& when, const std::string& where) -> void {
            if (when.op == When::Op::State) {
                const auto it = std::ranges::find_if(profile.states, [&](const auto& s) { return s.first == when.field; });
                if (it == profile.states.end()) {
                    Fail(where, fmt::format("{{ state = \"{}\" }}: no such [states] entry", when.field));
                }
                when.state_index = static_cast<u32>(it - profile.states.begin());
            }
            for (When& child : when.children) {
                self(self, child, where);
            }
        };
        for (Camera& camera : profile.cameras) {
            resolve(resolve, camera.when, fmt::format("game.toml camera '{}'", camera.id));
        }
        for (auto& [name, when] : profile.states) {
            resolve(resolve, when, fmt::format("address file state '{}'", name));
        }
        if (!profile.flat_state.empty()) {
            const auto it = std::ranges::find_if(profile.states, [&](const auto& s) { return s.first == profile.flat_state; });
            if (it == profile.states.end()) {
                Fail("game.toml [render]", fmt::format("flat_state '{}' is not an address-file [states] entry",
                                                       profile.flat_state));
            }
            profile.flat_state_index = static_cast<int>(it - profile.states.begin());
        }
    } catch (const ProfileError& e) {
        error = e.what();
        return std::nullopt;
    } catch (const std::exception& e) {
        error = fmt::format("profile load failed: {}", e.what());
        return std::nullopt;
    }
    return profile;
}

} // namespace VIZR::Profile
