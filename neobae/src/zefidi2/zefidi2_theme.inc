/* zefidi2 ImGui theme — compact player palette.
 *
 * Cool slate chrome with teal accent (distinct from nbeditor purple).
 * Include after imgui.h. Call Zefidi2Theme::ApplyImGuiStyle() after StyleColorsDark().
 */

namespace Zefidi2Theme
{

inline ImVec4 Accent()
{
    return ImVec4(0.22f, 0.72f, 0.68f, 1.00f);
}
inline ImVec4 AccentHover()
{
    return ImVec4(0.32f, 0.82f, 0.76f, 1.00f);
}
inline ImVec4 AccentActive()
{
    return ImVec4(0.14f, 0.55f, 0.52f, 1.00f);
}
inline ImVec4 FrameBg()
{
    return ImVec4(0.10f, 0.14f, 0.16f, 0.70f);
}
inline ImVec4 TitleBgActive()
{
    return ImVec4(0.10f, 0.16f, 0.18f, 1.00f);
}
inline ImVec4 Border()
{
    return ImVec4(0.28f, 0.42f, 0.44f, 0.55f);
}
inline ImVec4 SliderGrab()
{
    return ImVec4(0.20f, 0.62f, 0.58f, 1.00f);
}

inline ImVec4 ChannelOpen()
{
    return ImVec4(0.16f, 0.22f, 0.24f, 1.0f);
}
inline ImVec4 ChannelOpenHover()
{
    return ImVec4(0.22f, 0.32f, 0.34f, 1.0f);
}
inline ImVec4 ChannelOpenActive()
{
    return ImVec4(0.12f, 0.18f, 0.20f, 1.0f);
}
inline ImVec4 ChannelSolo()
{
    return ImVec4(0.18f, 0.48f, 0.32f, 1.0f);
}
inline ImVec4 ChannelSoloHover()
{
    return ImVec4(0.24f, 0.58f, 0.38f, 1.0f);
}
inline ImVec4 ChannelSoloActive()
{
    return ImVec4(0.14f, 0.40f, 0.26f, 1.0f);
}
inline ImVec4 ChannelMuted()
{
    return ImVec4(0.42f, 0.20f, 0.20f, 1.0f);
}
inline ImVec4 ChannelMutedHover()
{
    return ImVec4(0.52f, 0.26f, 0.26f, 1.0f);
}
inline ImVec4 ChannelMutedActive()
{
    return ImVec4(0.36f, 0.16f, 0.16f, 1.0f);
}

inline ImU32 ScopeBg()
{
    return IM_COL32(12, 18, 20, 255);
}
inline ImU32 ScopeBorder()
{
    return IM_COL32(50, 110, 105, 255);
}
inline ImU32 ScopeWave()
{
    return IM_COL32(90, 230, 210, 255);
}
inline ImU32 SeekFillLo()
{
    return IM_COL32(30, 140, 130, 255);
}
inline ImU32 SeekFillHi()
{
    return IM_COL32(80, 220, 200, 255);
}
inline ImU32 SeekTrack()
{
    return IM_COL32(28, 36, 40, 255);
}

inline void ApplyImGuiStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *c = style.Colors;
    const ImVec4 accent = Accent();
    const ImVec4 accent_hi = AccentHover();
    const ImVec4 accent_lo = AccentActive();
    const ImVec4 frame = FrameBg();

    /* Compact density */
    style.WindowPadding = ImVec2(8.0f, 6.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(6.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 8.0f;
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.ChildRounding = 3.0f;
    style.PopupRounding = 4.0f;

    c[ImGuiCol_Border] = Border();
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.11f, 0.12f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.12f, 0.14f, 0.98f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_FrameBgActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgActive] = TitleBgActive();
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = SliderGrab();
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_ButtonHovered] = accent_hi;
    c[ImGuiCol_ButtonActive] = accent_lo;
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.30f, 0.32f, 0.60f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.60f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(accent.x, accent.y, accent.z, 0.90f);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_Tab] = ImLerp(c[ImGuiCol_Header], c[ImGuiCol_TitleBgActive], 0.80f);
    c[ImGuiCol_TabSelected] = ImLerp(c[ImGuiCol_HeaderActive], c[ImGuiCol_TitleBgActive], 0.55f);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_HeaderActive];
    c[ImGuiCol_TabDimmed] = ImLerp(c[ImGuiCol_Tab], c[ImGuiCol_TitleBg], 0.80f);
    c[ImGuiCol_TabDimmedSelected] = ImLerp(c[ImGuiCol_TabSelected], c[ImGuiCol_TitleBg], 0.40f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
}

} /* namespace Zefidi2Theme */
