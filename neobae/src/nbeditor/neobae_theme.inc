/* NeoBAE Editor ImGui theme — tweak colors here.
 *
 * Brand base from neobae.ico ≈ #501050; accents are lifted for dark UI.
 * Include after imgui.h. Call NeoBaeTheme::ApplyImGuiStyle() after StyleColorsDark().
 */

namespace NeoBaeTheme
{

/* ---- Palette (edit these) ---- */

/* Primary interactive accent (buttons / headers / tabs / checkmarks). */
inline ImVec4 Accent()
{
    return ImVec4(0.62f, 0.32f, 0.72f, 1.00f);
}
inline ImVec4 AccentHover()
{
    return ImVec4(0.72f, 0.40f, 0.82f, 1.00f);
}
inline ImVec4 AccentActive()
{
    return ImVec4(0.48f, 0.20f, 0.58f, 1.00f);
}

/* Frames, title bars, muted chrome. */
inline ImVec4 FrameBg()
{
    return ImVec4(0.22f, 0.10f, 0.26f, 0.54f);
}
inline ImVec4 TitleBgActive()
{
    return ImVec4(0.22f, 0.10f, 0.26f, 1.00f);
}
inline ImVec4 Border()
{
    return ImVec4(0.48f, 0.32f, 0.55f, 0.50f);
}
inline ImVec4 SliderGrab()
{
    return ImVec4(0.55f, 0.28f, 0.65f, 1.00f);
}
inline ImVec4 SeparatorHover()
{
    return ImVec4(0.45f, 0.18f, 0.55f, 0.78f);
}
inline ImVec4 SeparatorActive()
{
    return ImVec4(0.45f, 0.18f, 0.55f, 1.00f);
}

/* Channel mute pad — open (not muted / not solo). */
inline ImVec4 ChannelOpen()
{
    return ImVec4(0.28f, 0.18f, 0.34f, 1.0f);
}
inline ImVec4 ChannelOpenHover()
{
    return ImVec4(0.38f, 0.24f, 0.46f, 1.0f);
}
inline ImVec4 ChannelOpenActive()
{
    return ImVec4(0.22f, 0.14f, 0.28f, 1.0f);
}

/* Channel mute pad — solo / muted (status colors, not brand accents). */
inline ImVec4 ChannelSolo()
{
    return ImVec4(0.22f, 0.48f, 0.28f, 1.0f);
}
inline ImVec4 ChannelSoloHover()
{
    return ImVec4(0.28f, 0.58f, 0.34f, 1.0f);
}
inline ImVec4 ChannelSoloActive()
{
    return ImVec4(0.18f, 0.40f, 0.24f, 1.0f);
}
inline ImVec4 ChannelMuted()
{
    return ImVec4(0.45f, 0.22f, 0.20f, 1.0f);
}
inline ImVec4 ChannelMutedHover()
{
    return ImVec4(0.55f, 0.28f, 0.25f, 1.0f);
}
inline ImVec4 ChannelMutedActive()
{
    return ImVec4(0.40f, 0.18f, 0.16f, 1.0f);
}

/* Links / alias text fallbacks. */
inline ImVec4 LinkText()
{
    return ImVec4(0.72f, 0.45f, 0.90f, 1.0f);
}
inline ImVec4 AliasTextFallback()
{
    return ImVec4(0.85f, 0.70f, 0.95f, 1.0f);
}

/* Player oscilloscope (packed ABGR via IM_COL32). */
inline ImU32 ScopeBg()
{
    return IM_COL32(22, 14, 28, 255);
}
inline ImU32 ScopeBorder()
{
    return IM_COL32(140, 80, 165, 255);
}
inline ImU32 ScopeWave()
{
    return IM_COL32(118, 226, 195, 255);
}

/* ---- Apply to ImGui style ---- */

inline void ApplyImGuiStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();
    ImVec4 *c = style.Colors;
    const ImVec4 accent = Accent();
    const ImVec4 accent_hi = AccentHover();
    const ImVec4 accent_lo = AccentActive();
    const ImVec4 frame = FrameBg();

    c[ImGuiCol_Border] = Border();
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_FrameBgActive] = ImVec4(accent.x, accent.y, accent.z, 0.67f);
    c[ImGuiCol_TitleBgActive] = TitleBgActive();
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_CheckboxSelectedBg] =
        ImLerp(frame, ImVec4(accent.x, accent.y, accent.z, 0.40f), 0.65f);
    c[ImGuiCol_SliderGrab] = SliderGrab();
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = ImVec4(accent.x, accent.y, accent.z, 0.40f);
    c[ImGuiCol_ButtonHovered] = accent_hi;
    c[ImGuiCol_ButtonActive] = accent_lo;
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.31f);
    c[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.80f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_SeparatorHovered] = SeparatorHover();
    c[ImGuiCol_SeparatorActive] = SeparatorActive();
    c[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.67f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(accent.x, accent.y, accent.z, 0.95f);
    c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
    c[ImGuiCol_Tab] = ImLerp(c[ImGuiCol_Header], c[ImGuiCol_TitleBgActive], 0.80f);
    c[ImGuiCol_TabSelected] = ImLerp(c[ImGuiCol_HeaderActive], c[ImGuiCol_TitleBgActive], 0.60f);
    c[ImGuiCol_TabSelectedOverline] = c[ImGuiCol_HeaderActive];
    c[ImGuiCol_TabDimmed] = ImLerp(c[ImGuiCol_Tab], c[ImGuiCol_TitleBg], 0.80f);
    c[ImGuiCol_TabDimmedSelected] = ImLerp(c[ImGuiCol_TabSelected], c[ImGuiCol_TitleBg], 0.40f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.70f);
    c[ImGuiCol_TextLink] = c[ImGuiCol_HeaderActive];
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavCursor] = accent;
}

} /* namespace NeoBaeTheme */
