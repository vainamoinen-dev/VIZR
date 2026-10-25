// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// One theme,
// applied once after StyleColorsDark seeds every slot, so anything not named here still has a sane value.
// Palette: deep blue-black glass and cyan lines, with orange for warnings only, so a
// warning is never mistaken for a highlight.

#pragma once

#include <cmath>

#include <imgui.h>

namespace Vulkan::OpenXr::MenuStyle {

constexpr ImVec4 Rgb(unsigned hex, float alpha = 1.0f) {
    return ImVec4(static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(hex & 0xFF) / 255.0f, alpha);
}

/// True when the panel's swapchain is sRGB: ImGui's colours are then handed over linear.
inline bool& LinearOutput() {
    static bool linear = false;
    return linear;
}

/// Every colour the menu draws goes through here (theme, header, ring), so the sRGB rule is in one place.
inline ImVec4 Col(unsigned hex, float alpha = 1.0f) {
    ImVec4 c = Rgb(hex, alpha);
    if (LinearOutput()) {
        c.x = std::pow(c.x, 2.2f);
        c.y = std::pow(c.y, 2.2f);
        c.z = std::pow(c.z, 2.2f);
    }
    return c;
}

constexpr unsigned PanelBg = 0x070D12;     // visor glass, near black
constexpr unsigned SurfaceBg = 0x0D1820;   // child panels, popups
constexpr unsigned TroughBg = 0x112430;    // slider and input backgrounds
constexpr unsigned Raised = 0x163242;      // buttons at rest
constexpr unsigned RaisedHover = 0x1F4A60;
constexpr unsigned Selected = 0x1A3C50;    // selected header / tab
constexpr unsigned TextMain = 0xDDF4FF;
constexpr unsigned TextDim = 0x6E8C9A;
constexpr unsigned Accent = 0x4FD8FF;      // HUD cyan
constexpr unsigned AccentHot = 0x9BEBFF;
constexpr unsigned AccentDown = 0x2BA8D0;
constexpr unsigned Border = 0x1C3A4A;
constexpr unsigned Separator = 0x245468;
constexpr unsigned Warning = 0xFF7A1A;     // suit orange: warnings only

inline void Apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = Col(PanelBg, 0.94f);
    c[ImGuiCol_ChildBg] = Col(SurfaceBg);
    c[ImGuiCol_PopupBg] = Col(SurfaceBg, 0.98f);
    c[ImGuiCol_Text] = Col(TextMain);
    c[ImGuiCol_TextDisabled] = Col(TextDim);
    c[ImGuiCol_Border] = Col(Border);
    c[ImGuiCol_BorderShadow] = Col(0x000000, 0.0f);
    c[ImGuiCol_Separator] = Col(Separator);
    c[ImGuiCol_SeparatorHovered] = Col(Accent, 0.6f);
    c[ImGuiCol_SeparatorActive] = Col(Accent);
    c[ImGuiCol_FrameBg] = Col(TroughBg);
    c[ImGuiCol_FrameBgHovered] = Col(RaisedHover);
    c[ImGuiCol_FrameBgActive] = Col(Selected);
    c[ImGuiCol_TitleBg] = Col(SurfaceBg);
    c[ImGuiCol_TitleBgActive] = Col(Selected);
    c[ImGuiCol_TitleBgCollapsed] = Col(SurfaceBg);
    c[ImGuiCol_MenuBarBg] = Col(SurfaceBg);
    c[ImGuiCol_Button] = Col(Raised);
    c[ImGuiCol_ButtonHovered] = Col(RaisedHover);
    c[ImGuiCol_ButtonActive] = Col(AccentDown);
    c[ImGuiCol_Header] = Col(Selected);
    c[ImGuiCol_HeaderHovered] = Col(RaisedHover);
    c[ImGuiCol_HeaderActive] = Col(AccentDown, 0.85f);
    c[ImGuiCol_CheckMark] = Col(Accent);
    c[ImGuiCol_SliderGrab] = Col(Accent);
    c[ImGuiCol_SliderGrabActive] = Col(AccentHot);
    c[ImGuiCol_ScrollbarBg] = Col(PanelBg, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Col(Separator);
    c[ImGuiCol_ScrollbarGrabHovered] = Col(RaisedHover);
    c[ImGuiCol_ScrollbarGrabActive] = Col(Accent);
    c[ImGuiCol_Tab] = Col(Raised);
    c[ImGuiCol_TabHovered] = Col(RaisedHover);
    c[ImGuiCol_TabSelected] = Col(Selected);
    c[ImGuiCol_TabSelectedOverline] = Col(Accent);
    c[ImGuiCol_ResizeGrip] = Col(PanelBg, 0.0f); // the panel is locked
    c[ImGuiCol_ResizeGripHovered] = Col(PanelBg, 0.0f);
    c[ImGuiCol_ResizeGripActive] = Col(PanelBg, 0.0f);
    c[ImGuiCol_NavCursor] = Col(Accent);

    // Square, technical edges suit the visor look and stay crisp when the panel is seen at an angle.
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 2.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 0.0f;
    s.ScrollbarRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
}

} // namespace Vulkan::OpenXr::MenuStyle
