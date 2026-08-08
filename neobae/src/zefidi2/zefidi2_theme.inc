/* zefidi2 ImGui theme — compact player palette.
 *
 * Accent: Windows DWM AccentColor when available (same source as classic zefidi);
 * otherwise NeoBAE purple (neobae.ico ≈ #501050, lifted for dark UI).
 *
 * Include after imgui.h. Call DetectSystemAccent() then ApplyImGuiStyle()
 * after StyleColorsDark().
 */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Zefidi2Theme
{

/* NeoBAE brand fallback (matches nbeditor). */
inline ImVec4 DefaultAccent()
{
    return ImVec4(0.62f, 0.32f, 0.72f, 1.00f);
}

inline ImVec4 &AccentStorage()
{
    static ImVec4 s = DefaultAccent();
    return s;
}

inline ImVec4 Accent()
{
    return AccentStorage();
}

inline ImVec4 Lighten(const ImVec4 &c, float amount)
{
    return ImVec4(ImMin(c.x + amount, 1.0f),
                  ImMin(c.y + amount, 1.0f),
                  ImMin(c.z + amount, 1.0f),
                  c.w);
}

inline ImVec4 Darken(const ImVec4 &c, float amount)
{
    return ImVec4(ImMax(c.x - amount, 0.0f),
                  ImMax(c.y - amount, 0.0f),
                  ImMax(c.z - amount, 0.0f),
                  c.w);
}

inline ImVec4 AccentHover()
{
    return Lighten(Accent(), 0.10f);
}
inline ImVec4 AccentActive()
{
    return Darken(Accent(), 0.12f);
}

inline ImVec4 FrameBg()
{
    const ImVec4 a = Accent();
    return ImVec4(a.x * 0.28f, a.y * 0.28f, a.z * 0.28f, 0.55f);
}
inline ImVec4 TitleBgActive()
{
    const ImVec4 a = Accent();
    return ImVec4(a.x * 0.30f, a.y * 0.30f, a.z * 0.30f, 1.00f);
}
inline ImVec4 Border()
{
    const ImVec4 a = Accent();
    return ImVec4(a.x * 0.55f, a.y * 0.55f, a.z * 0.55f, 0.50f);
}
inline ImVec4 SliderGrab()
{
    return Darken(Accent(), 0.05f);
}

/* Channel pads: status colors stay fixed; open pads tint toward accent. */
inline ImVec4 ChannelOpen()
{
    const ImVec4 a = Accent();
    return ImVec4(0.10f + a.x * 0.18f, 0.10f + a.y * 0.18f, 0.10f + a.z * 0.18f, 1.0f);
}
inline ImVec4 ChannelOpenHover()
{
    return Lighten(ChannelOpen(), 0.08f);
}
inline ImVec4 ChannelOpenActive()
{
    return Darken(ChannelOpen(), 0.06f);
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

inline ImU32 ToU32(const ImVec4 &c, int a = 255)
{
    return IM_COL32(static_cast<int>(c.x * 255.0f + 0.5f),
                    static_cast<int>(c.y * 255.0f + 0.5f),
                    static_cast<int>(c.z * 255.0f + 0.5f),
                    a);
}

inline ImU32 ScopeBg()
{
    return IM_COL32(14, 14, 18, 255);
}
inline ImU32 ScopeBorder()
{
    return ToU32(Darken(Accent(), 0.15f));
}
inline ImU32 ScopeWave()
{
    /* Slightly lifted accent for waveform contrast on dark scope. */
    return ToU32(Lighten(Accent(), 0.20f));
}
inline ImU32 SeekFillLo()
{
    return ToU32(Darken(Accent(), 0.10f));
}
inline ImU32 SeekFillHi()
{
    return ToU32(Lighten(Accent(), 0.15f));
}
inline ImU32 SeekTrack()
{
    return IM_COL32(28, 28, 34, 255);
}

/* Resolve accent before ApplyImGuiStyle(). Safe on all platforms. */
inline void DetectSystemAccent()
{
    AccentStorage() = DefaultAccent();

#if defined(_WIN32)
    /* Same source as classic zefidi gui_theme.c: HKCU\...\DWM\AccentColor (AABBGGRR). */
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\DWM",
                      0,
                      KEY_READ,
                      &key) == ERROR_SUCCESS)
    {
        DWORD value = 0;
        DWORD type = 0;
        DWORD size = sizeof(value);
        const LONG ok = RegQueryValueExA(key, "AccentColor", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
        if (ok == ERROR_SUCCESS && type == REG_DWORD && size >= sizeof(DWORD))
        {
            const float r = static_cast<float>((value >> 0) & 0xFFu) / 255.0f;
            const float g = static_cast<float>((value >> 8) & 0xFFu) / 255.0f;
            const float b = static_cast<float>((value >> 16) & 0xFFu) / 255.0f;
            /* Reject near-black / near-white so UI stays readable. */
            const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (luma > 0.08f && luma < 0.92f)
            {
                AccentStorage() = ImVec4(r, g, b, 1.0f);
            }
        }
    }
#endif
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
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_FrameBgActive] = ImVec4(accent.x, accent.y, accent.z, 0.55f);
    c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
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
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.28f, 0.60f);
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
