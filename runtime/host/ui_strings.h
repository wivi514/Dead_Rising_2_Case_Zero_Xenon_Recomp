// The host-drawn UI's strings — the launcher and the in-game PC SETTINGS panel —
// in the language the player picked for the game's subtitles.
//
// WHY THIS EXISTS (2026-09-14, operator request). The game's own menus follow the
// SUBTITLES row (part 99: the Xbox console-language ID selects the str_XX.bcs
// bank), but the two menus this runtime draws itself — the launcher window and
// the settings panel over the Help & Options hub — were English whatever the
// player chose. They should read in the same language as the game they open.
//
// Both menus render with window.cpp's 5x7 bitmap font: upper-case Latin letters,
// digits and a little punctuation, no accents, no CJK. So the four Latin banks the
// disc carries — English, French, Italian, Spanish — are translated here in
// accent-less capitals (the way the launcher already spells FRANCAIS and ESPANOL),
// and JAPANESE and KOREAN fall back to English: a font that cannot draw the
// language must not pretend to. Every string is looked up at DRAW time, so the
// launcher re-labels itself the moment the SUBTITLES row is stepped.
#pragma once

enum class UiStr
{
    // launcher rows
    Play,
    NotInstalledYet,
    DisplayMode,
    Resolution,
    VSync,
    Shadows,
    Msaa,
    FpsCap,
    Fov,
    Subtitles,
    SkipIntroLogos,
    // shared values
    Window,
    Borderless,
    Fullscreen,
    Off,
    On,
    Low,
    Medium,
    High,
    RtLow,
    RtMedium,
    RtHigh,
    Default,
    // launcher lines
    LauncherHint,
    GameInstalled,
    DropPackage,
    ControlsPad,
    ControlsKeyboard,
    Installing,
    CopyingPackage,
    Unpacking,         // printf: "%u OF %u MB"
    InstalledPressEnter,
    InstallFailed,     // prefix
    NotAPackage,       // prefix; the first bytes follow
    CouldNotCopy,      // prefix; the OS error follows
    // the settings panel
    PcSettings,
    PanelHintKeyboard,
    PanelHintPad,
    Shadow,
    FrameCap,
    FieldOfView,
    MouseSens,
    Exposure,
    FooterApplyResolution,
    FooterMsaaNextLaunch,
    FooterRtOff,
    FooterNoRayQuery,
    FooterNoRtCache,
    FooterShadowLive,
    FooterShadowInert,
    Count
};

// The string for the CURRENT subtitle language (host/settings.cpp's
// Settings_Language()). Never null; never empty.
const char* UiText(UiStr id);

// Which language UiText answers in: an Xbox console-language ID of 1 (English),
// 4 (French), 6 (Italian) or 5 (Spanish) — never Japanese or Korean, which map to
// English here. For the log line that says so.
int UiTextLanguage();
