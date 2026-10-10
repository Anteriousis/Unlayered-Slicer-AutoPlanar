#!/usr/bin/env python3
"""WCAG contrast audit of the wx palette in both themes.

Reads the light -> dark colour table (gDarkColors in src/slic3r/GUI/Widgets/StateColor.cpp)
and the accent ramp (src/slic3r/GUI/AccentColor.hpp), then evaluates a checked-in list of
foreground/background ROLES: each role names its light-theme colours, and the dark-theme
pair is whatever the table maps them to (accent colours follow AccentColor.hpp's own rules).
The bar is WCAG AA: 4.5:1 for text, 3:1 for non-text components.

Usage:  python tools/theme/contrast-audit.py [--check]
    --check   exit 1 if any role fails in either theme (CI gate); still prints the table
Change a palette value and run this before and after; add a role whenever a new pairing
appears in the widgets. The roles are the documented pairings, not every literal in the
tree -- hard-coded colours outside the table are found by the screenshot walk, not here.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
STATE_COLOR = os.path.join(ROOT, "src/slic3r/GUI/Widgets/StateColor.cpp")
ACCENT = os.path.join(ROOT, "src/slic3r/GUI/AccentColor.hpp")

TEXT, UI = "text", "ui"
BAR = {TEXT: 4.5, UI: 3.0}

# (role, foreground, background, kind). "$Name" refers to an AccentColor.hpp constant.
ROLES = [
    ("Parameter label on window",             "#363636", "#FFFFFF", TEXT),
    ("Parameter label on sidebar panel",      "#363636", "#CECECE", TEXT),
    ("Parameter label on titlebar gradient",  "#363636", "#F8F8F8", TEXT),
    ("Softer text on window",                 "#323A3D", "#FFFFFF", TEXT),
    ("Column header text on window",          "#303A3C", "#FFFFFF", TEXT),
    ("Sidebar tab text on side tabbar",       "#363636", "#FEFFFF", TEXT),
    ("Button text on button",                 "#262E30", "#DFDFDF", TEXT),
    ("Button text on hovered button",         "#262E30", "#D4D4D4", TEXT),
    ("Dimmed button text on button",          "#6B6A6A", "#DFDFDF", TEXT),
    ("Input side text on window",             "#6B6B6A", "#FFFFFF", TEXT),
    ("Disabled text on window",               "#6B6B6B", "#FFFFFF", TEXT),
    ("Slice/Print label on accent",           "#FEFEFE", "$Base",   TEXT),
    ("Slice/Print label on accent hover",     "#FEFEFE", "$Hover",  TEXT),
    ("Slice/Print label on accent pressed",   "#FEFEFE", "$Pressed", TEXT),
    ("Slice/Print disabled label on pill",    "#D0D0D1", "#4F585A", TEXT),
    ("Accent vs window",                      "$Base",   "#FFFFFF", UI),
    ("Accent vs button",                      "$Base",   "#DFDFDF", UI),
    ("Checked-item tint vs window",           "$Tint25", "#FFFFFF", UI),
    ("Focused-combo tint vs window",          "$Tint10", "#FFFFFF", UI),
    ("Input border vs window",                "#DBDBDB", "#FFFFFF", UI),
    ("Separator vs window",                   "#EEEEEE", "#FFFFFF", UI),
    ("Selected tab bg vs tab bar",            "#CECECE", "#3B4446", UI),
    ("Titlebar gradient vs window",           "#F8F8F8", "#FFFFFF", UI),
    ("Disabled element bg vs window",         "#F0F0F1", "#FFFFFF", UI),
    ("Dimmed elements vs button",             "#ACACAC", "#DFDFDF", UI),
    ("Toggle track vs window",                "#D9D9D9", "#FFFFFF", UI),
]

# ImGui does not consult the table; its per-theme literals live in ImGuiWrapper.cpp.
IMGUI_ROLES = [
    ("ImGui text on window",         ("#323A3D", "#FFFFFF"), ("#E1E1E1", "#232327"), TEXT),  # dark text is white at 0.88 alpha
    ("ImGui legend subdued text",    ("#6E6E6E", "#FFFFFF"), ("#ACACAC", "#232327"), TEXT),  # GCodeViewer::render_legend
    ("ImGui check mark on window",   ("#323A3D", "#FFFFFF"), ("#FFFFFF", "#232327"), UI),
    ("Top bar text on top bar",      ("#FFFFFF", "#3B4446"), ("#FFFFFF", "#1E1E21"), TEXT),  # wxSYS_COLOUR_HIGHLIGHTTEXT, not table-mapped
    ("Modified-value label",         ("#C9500E", "#FFFFFF"), ("#F1754E", "#232327"), TEXT),  # GUI_App::init_label_colours
]


def read_table():
    src = open(STATE_COLOR, encoding="utf-8").read()
    block = src[src.index("gDarkColors{"):]
    block = block[:block.index("};")]
    rows = re.findall(r'\{"(#[0-9A-Fa-f]{6})",\s*"(#[0-9A-Fa-f]{6})"\}', block)
    return {k.upper(): v.upper() for k, v in rows}


def read_accent():
    src = open(ACCENT, encoding="utf-8").read()
    consts = dict(re.findall(r'\*(\w+)\s*=\s*"(#[0-9A-Fa-f]{6})"', src))
    light = {k: v.upper() for k, v in consts.items()}
    # Theme rules from AccentColor.hpp: the ramp is theme-independent except Hover; tints follow the theme.
    dark = dict(light)
    for name in ("Hover", "Tint25", "Tint10"):
        if name + "Dark" in light:
            dark[name] = light[name + "Dark"]
    return light, dark


def luminance(hexcol):
    r, g, b = (int(hexcol[i:i + 2], 16) / 255.0 for i in (1, 3, 5))
    lin = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def ratio(fg, bg):
    lf, lb = luminance(fg), luminance(bg)
    hi, lo = max(lf, lb), min(lf, lb)
    return (hi + 0.05) / (lo + 0.05)


# Theme files (src/slic3r/GUI/Theme.cpp) name roles by id; the code keys them by light colour.
ROLE_KEYS = {
    "window_background": "#FFFFFF", "panel_title_strips": "#F8F8F8", "tab_bar": "#3B4446", "top_bar": "#262E31",
    "selected_tab": "#CECECE", "buttons": "#DFDFDF", "input_borders": "#DBDBDB",
    "input_background": "#FFFEFF", "labels": "#363636", "button_input_text": "#262E30",
    "disabled_text": "#6B6B6B", "separators": "#EEEEEE", "accent": "#767676",
}


def read_theme(path):
    """A theme file's overrides as {light key: colour} plus its name and base."""
    import json
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    overrides = {}
    for role, value in data.get("colors", {}).items():
        key = ROLE_KEYS.get(role)
        if key:
            overrides[key] = value.upper()
            if key == "#F8F8F8":  # the title strip gradient has two rows; the app keeps them together
                overrides["#F1F1F1"] = value.upper()
    return data.get("name", path), data.get("base", "light") == "dark", overrides


def themed_accent(name, accent, overrides, dark):
    """The accent ramp for a theme that sets the accent role, derived as ThemeManager::apply_to_state does."""
    base = overrides.get("#767676") if overrides else None
    if base is None:
        return accent[name]
    r, g, b = int(base[1:3], 16), int(base[3:5], 16), int(base[5:7], 16)
    light = (r + g + b) // 3 > 160
    def shift(d):
        return "#%02X%02X%02X" % tuple(max(0, min(255, c + d)) for c in (r, g, b))
    if name == "Base":
        return base
    if name in ("Hover", "HoverDark"):
        return shift(-18 if light else (50 if dark else 18))
    if name == "Pressed":
        return shift(-40 if light else -26)
    return accent[name]


def resolve(token, accent, table, dark, overrides=None):
    if token.startswith("$"):
        return themed_accent(token[1:], accent, overrides, dark)
    token = token.upper()
    if overrides and token in overrides:
        return overrides[token]
    return table.get(token, token) if dark else token


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if any role fails")
    ap.add_argument("--theme", metavar="FILE", help="evaluate a theme file (.json) on top of the built-in colours; "
                    "its overrides apply to the column of its base (auto-contrast text is not simulated)")
    args = ap.parse_args()

    table = read_table()
    accent_light, accent_dark = read_accent()
    ov_light = ov_dark = None
    if args.theme:
        name, theme_dark, overrides = read_theme(args.theme)
        if theme_dark:
            ov_dark = overrides
        else:
            ov_light = overrides
        print(f"theme: {name} (base {'dark' if theme_dark else 'light'}, {len(overrides)} override(s)); "
              f"read the {'dark' if theme_dark else 'light'} column")
    rows = []
    for role, fg, bg, kind in ROLES:
        lf, lb = resolve(fg, accent_light, table, False, ov_light), resolve(bg, accent_light, table, False, ov_light)
        df, db = resolve(fg, accent_dark, table, True, ov_dark), resolve(bg, accent_dark, table, True, ov_dark)
        rows.append((role, kind, (lf, lb), (df, db)))
    for role, light, dark, kind in IMGUI_ROLES:
        rows.append((role, kind, light, dark))

    failures = 0
    print(f"{'role':<38} {'kind':<4} {'light fg/bg':<16} {'ratio':>6}   {'dark fg/bg':<16} {'ratio':>6}")
    for role, kind, (lf, lb), (df, db) in rows:
        rl, rd = ratio(lf, lb), ratio(df, db)
        bar = BAR[kind]
        ml = " " if rl >= bar else "!"
        md = " " if rd >= bar else "!"
        failures += (ml == "!") + (md == "!")
        print(f"{role:<38} {kind:<4} {lf}/{lb:<8} {rl:5.2f}{ml}   {df}/{db:<8} {rd:5.2f}{md}")
    print(f"\n{failures} failing pairing(s); bar {BAR[TEXT]}:1 text, {BAR[UI]}:1 components; '!' marks a failure")
    return 1 if (args.check and failures) else 0


if __name__ == "__main__":
    sys.exit(main())
