# Themes

A theme is a named set of interface colours the user can edit live, save, switch between and
share as a file. The code lives in `src/slic3r/GUI/Theme.{hpp,cpp}` (`Theme`, `ThemeManager`)
and is exposed in Preferences > Appearance.

## Model

Every theme derives from one of the two built-in bases, Light or Dark, and stores only what
differs from that base:

- interface colour roles (window background, panel title strips, tab bar, top bar, selected tab,
  buttons, input borders, input background, labels, button and input text, disabled text,
  separators, accent), keyed in files by a readable id and in code by the light-theme colour the
  widgets use (the `StateColor` table key). The accent is the grey of the selected main tab,
  the Slice/Print buttons, toggles and the plate icons; its hover and pressed states are
  derived from the chosen colour, and the ImGui twins (`ImGuiWrapper::COL_ORCA*`) follow it;
- the 3D view background, the build plate itself (the bed model), the grid lines of the current
  plate, and the background of the toolbars drawn inside the 3D view (a flat quad in place of
  the shipped texture for the icon toolbars; the ImGui toolbar windows mirror the same colour);
- the two parameter-label colours (system value, modified value);
- the keyboard focus outline colour;
- whether text is auto-contrasted (see below).

The base decides `dark_color_mode`. Roles are named by id rather than by hex key so that a file
stays readable and survives changes to the colour table.

## Where themes live

- Built-in `Light` and `Dark`: in code, no overrides, read-only.
- Bundled presets: `resources/themes/*.json`, read-only, loaded at startup (`High contrast
  light`, `High contrast dark`, `Neon`).
- User themes: `<datadir>/themes/<slug>.json`.
- The theme in effect for each base is named in app-config `theme_light` / `theme_dark`
  (empty = the built-in). `color_scheme` keeps its meaning: System follows the desktop and
  picks the slot for that base; Light / Dark force one. Choosing a theme of the other base
  switches the colour scheme to it unless System is selected.

Editing a read-only theme forks it into an unsaved working copy ("<name> (modified)"); Save on
such a copy asks for a name. An unsaved working copy is persisted as
`<datadir>/themes/.unsaved-<base>.json` so it survives a restart and is discarded by Revert,
by Save, or by selecting another theme of that base.

A file's `plate` key from before the grid/model split is read as `plate_grid`, which is what it
coloured in effect.

Import adds a theme file as a user theme (a read-only name gets " (imported)" appended) and
selects it; Export writes the working copy under its own name. Files are plain JSON, version 1.

## Applying a theme

`ThemeManager::apply_to_state()` is the one place colours leave the theme:

1. `StateColor` theme overrides for the base (widgets resolve their colours through
   `darkModeColorFor()` at paint time, so those follow at the next repaint), including the
   table colours that are a role's colour on another surface (`role_aliases()`: the second
   title-strip row, the settings tab strip and Device page for the window background, the
   checked dropdown row and selected settings tab for the selected tab);
2. the auto-contrast pass: for each text role the theme did not set, the resolved text colour
   is pushed lighter or darker, hue kept, until it reads at 4.5:1 on each background it sits
   on (`StateColor::EnsureContrast`); the fixed label and button-label colours get the same
   treatment in `GUI_App::init_label_colours`;
3. the 3D view overrides (`GLCanvas3D` background, `Bed3D` model, `PartPlate` grid, `GLToolbar`
   background);
4. the plate icons, recoloured at load (the icon background takes the buttons colour, the
   glyph the button-text colour, the hovered glyph the focus outline colour) when the theme
   sets those roles; the home page, which gets
   a style element with the window, label, disabled-text and separator colours the theme
   sets, on top of its own stylesheets;
5. the focus outline colour; the ImGui mirrors: the window background, the toolbar band, and
   on the dark base the translucent overlay background (legend, tool position, plate labels,
   tooltips), with the notification cards and the plate selector strip reading the same value.

Windows that were coloured once at construction do not consult the table again, so a change
is followed by `GUI_App::retheme_live()`: it snapshots the resolved colour of every role before
the change and afterwards gives every window still carrying an old colour (background or
foreground) the new one. That is a preview only: a window whose colour was derived rather than
copied, or two roles that resolve to the same colour, cannot be told apart by colour matching.
`ThemeManager::edit_generation()` counts colour and flag edits (not switching between whole
themes, which the preview reproduces exactly), and Preferences rebuilds the main window when it
closes if the count moved (the route font scale and language take), so the state after editing
is constructed from the theme itself and cannot drift. Plain panels are built with literal
colours and, on the light base, never consult the table, so once a window tree is complete
(startup, the rebuilt frame, Preferences) `GUI_App::retheme_from_base()` runs the same pass with
the base's built-in colours as the "before" state. The progress dialog is shown before the old
window is torn down, and the open project is carried across the rebuild through a temporary 3MF
(written before teardown, one file per process; reloaded afterwards through `Plater::load_project`
like a crash restore, so plates, per-plate settings and positions survive; name and dirty state
restored). If the file cannot be written the window is left as previewed rather than rebuilt. The preview itself matches on every colour a role has resolved
to during the session, picks by a fixed role priority when a colour is shared, leaves widgets
that paint from the table alone, and every role has a colour of its own in both bases and across
them (the input background's dark value differs from the window's by one unit; the top bar's dark
value is not the button-text key) so no two roles can be confused, even after a base flip. A theme
value equal to what another role resolves to is nudged by one unit when applied, for the same
reason.

Theme values are kept out of the colour table's namespace when they are applied
(`StateColor::AvoidTableCollision`): on the dark base a value equal to a light key, on the light
base a value equal to a dark value, is nudged by one unit per channel. Re-resolving a window's
current colour through the table (the classic dark-mode pass) is then safe: a literal from
construction maps, an applied theme colour passes through unchanged, and a black window is not
sent through the black-text row. The resolver also returns an override's own value unchanged.

## Migration

The loose app-config keys that preceded themes (`theme_<base>_<hex>`, `view_bg_color_<base>`,
`plate_color_<base>`, `label_clr_*`, `focus_ring_color`, `auto_contrast_text`) are folded into
a `Custom light` / `Custom dark` user theme on first start, selected in their slot, and removed.

## Checking a theme

`tools/theme/contrast-audit.py --theme <file>` evaluates the WCAG pairings with the file's
overrides applied to the column of its base. The auto-contrast pass is not simulated there.
