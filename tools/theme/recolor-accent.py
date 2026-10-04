#!/usr/bin/env python3
"""Re-derive the Unlayered grayscale accent from upstream OrcaSlicer's teal.

Upstream draws its accent as #009688 (plus the siblings #26a69a, #4db6ac, #33aba1).
This script rewrites those to a neutral grey ramp, in place, across resources/images
and resources/web. It is idempotent: the source and target hex sets are disjoint, so a
second run is a no-op.

Why bake into the assets instead of remapping at load time
----------------------------------------------------------
BitmapCache::load_svg() does have a runtime replace table, but four other loaders --
GLTexture (3D toolbar, bed textures), IMTexture (layer slider), IconManager (gizmo
atlas) and BitmapCache::load_svg2 (filament swatches) -- never call it. Anything that
relied on the runtime table rendered unrecoloured on those surfaces. Baking is the only
approach that is correct for every loader.

Upstream merges stay cheap because this is mechanical:

    git checkout --theirs -- resources/images/ resources/web/
    python tools/theme/recolor-accent.py
    git add resources/images/ resources/web/

Theme handling
--------------
* A file with a _dark.svg twin gets the dark tier baked straight in -- the loader picks
  the file by name, so this works even on the no-hook paths.
* A file with no _dark twin gets the light tier, and BitmapCache::load_svg() maps
  light -> dark at load time. Its key set must stay in sync with LIGHT below.

Usage:  python tools/theme/recolor-accent.py [--check] [--audit]
    --check   exit 1 if anything would change (CI gate); writes nothing
    --audit   print the skip list with each file's accent-hex count, then exit
"""

import fnmatch
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGES = os.path.join(ROOT, 'resources', 'images')
WEB = os.path.join(ROOT, 'resources', 'web')

# Upstream accent and its siblings. Matched case-insensitively: the assets spell the
# siblings in lowercase and #009688 in uppercase, and the old runtime table missed the
# lowercase ones for exactly that reason.
SOURCES = ('009688', '26a69a', '4db6ac', '33aba1')

# Target ramp. INVARIANT: LIGHT and DARK must stay disjoint -- BitmapCache applies its
# replacements cumulatively in key order, so an overlap would make the result depend on
# lexicographic ordering. They must also not collide with any colour already present in
# the assets, or the dark remap would catch unrelated artwork.
LIGHT = {'base': '#5D5D5D', 'strong': '#3D3D3D'}
DARK = {'base': '#B8B8B8', 'strong': '#DADADA'}

# Very light green washes used as background fills rather than as the accent proper.
# They are too desaturated for the ramp above but still read green next to grey chrome,
# so they get flat neutral equivalents of the same lightness. Same value in both themes:
# upstream never remapped these for dark mode either, so this preserves parity.
TINTS = {
    'e5f0ee': '#ECECEC',  # old ORCA 10% tint -- canvas_{menu,zoom}_hover
    'ebf9f0': '#F2F2F2',  # pipe_of_* background wash
    'ebffd7': '#F5F5F5',  # pipe_of_empty
}
TINT_RE = re.compile('#(' + '|'.join(TINTS) + ')', re.IGNORECASE)

# The mark drawn ON TOP of the accent fill in an on-state icon: the checkbox tick, the
# toggle thumb, the capsule glyph. Upstream draws these near-white, which works only
# while the fill underneath is dark. In dark mode the fill inverts to #DADADA and a
# near-white glyph disappears entirely. Collapsing them onto one token lets
# BitmapCache::load_svg() flip the glyph to #2D2D31 exactly when it flips the fill.
# Applied ONLY to files matching recolor-strong.globs -- these same near-whites are
# ordinary artwork everywhere else.
GLYPH = '#FCFCFC'
GLYPH_RE = re.compile(r'#(?:fff|ffffff|e6e6e6|f7f7f7)\b', re.IGNORECASE)

# resources/web has no BitmapCache remap, and its pages theme themselves through
# prefers-color-scheme, so web art needs the THEME-INDEPENDENT accent (Accent::Base in
# GUI/AccentColor.hpp) rather than the light half of the icon ramp -- same reasoning as
# the GLTexture / IMTexture / IconManager paths.
WEB_ACCENT = '#767676'

# A few homepage tiles are PNG, not SVG (the "new project" / "open project" icons), so
# the text sweep cannot reach them. They are flat single-colour glyphs, so an exact RGB
# swap is safe. Requires Pillow; skipped with a warning if it is not installed.
PNG_TARGETS = {
    'homepage/img/i4.png': [((0, 150, 136), (118, 118, 118))],  # new project
    'homepage/img/i5.png': [((0, 150, 136), (118, 118, 118))],  # open project
}

# Brand assets the fork's own branding commit missed. The PNG/ICO logos under
# resources/images were all replaced with the grayscale Unlayered mark, but this one --
# used for the home screen's header and shortcut icons, and by guide/1 -- was left as the
# coloured Orca whale. resources/web has no other fork divergence, so a merge that takes
# upstream wholesale would silently restore it; re-copying here keeps that reversible.
BRAND_COPIES = {
    # dest, relative to resources/web        source, relative to resources/images
    'image/logo.png': 'UnlayeredSlicer_154.png',  # both 154x154
}


def load_globs(name):
    path = os.path.join(HERE, name)
    with open(path, encoding='utf-8') as f:
        return [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith('#')]


def matches(basename, globs):
    return any(fnmatch.fnmatch(basename, g) for g in globs)


def read(path):
    with open(path, encoding='utf-8', errors='surrogateescape', newline='') as f:
        return f.read()


def write(path, text):
    with open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
        f.write(text)


ACCENT_RE = re.compile('#(?:' + '|'.join(SOURCES) + ')', re.IGNORECASE)


def audit(skip):
    print('Skip list -- files excluded from the accent sweep:\n')
    seen = set()
    for g in skip:
        for name in sorted(os.listdir(IMAGES)):
            if name in seen or not fnmatch.fnmatch(name, g):
                continue
            seen.add(name)
            n = len(ACCENT_RE.findall(read(os.path.join(IMAGES, name))))
            print(f'  {name:<34} accent_hexes={n:<5} matched_by={g}')
    if not seen:
        print('  (nothing matched)')


def sweep(check):
    skip = load_globs('recolor-skip.globs')
    strong = load_globs('recolor-strong.globs')
    changed, subs = [], 0

    for name in sorted(os.listdir(IMAGES)):
        if not name.endswith('.svg') or matches(name, skip):
            continue
        path = os.path.join(IMAGES, name)
        text = read(path)
        is_dark = name.endswith('_dark.svg')
        is_strong = matches(name, strong)
        # Note the glyph clause: on a re-run over already-baked assets the accent hex is
        # gone, so testing ACCENT_RE alone would skip the file before the glyph pass.
        if not (ACCENT_RE.search(text) or TINT_RE.search(text)
                or (is_strong and not is_dark and GLYPH_RE.search(text))):
            continue
        ramp = DARK if is_dark else LIGHT
        target = ramp['strong' if is_strong else 'base']
        new, n = ACCENT_RE.subn(target, text)
        new, t = TINT_RE.subn(lambda m: TINTS[m.group(1).lower()], new)
        n += t
        # Light on-state icons only: the _dark twins already carry a dark fill, so their
        # near-white glyph is correct as drawn and must be left alone.
        if is_strong and not is_dark:
            new, k = GLYPH_RE.subn(GLYPH, new)
            n += k
        if n:
            subs += n
            changed.append(name)
            if not check:
                write(path, new)

    # resources/web -- same mapping, no skip list, no _dark filename convention (the
    # web pages theme themselves through prefers-color-scheme in include/global.css).
    for dirpath, _, names in os.walk(WEB):
        for name in names:
            if not name.endswith('.svg'):
                continue
            path = os.path.join(dirpath, name)
            text = read(path)
            new, n = ACCENT_RE.subn(WEB_ACCENT, text)
            new, k = re.subn(re.escape(LIGHT['base']), WEB_ACCENT, new, flags=re.IGNORECASE)
            n += k
            if n:
                subs += n
                changed.append(os.path.relpath(path, ROOT))
                if not check:
                    write(path, new)

    # Brand assets missed by the fork's branding commit.
    import filecmp
    import shutil
    for dest_rel, src_name in BRAND_COPIES.items():
        dest = os.path.join(WEB, *dest_rel.split('/'))
        src = os.path.join(IMAGES, src_name)
        if not os.path.isfile(src):
            print(f'  WARNING: brand source missing: {src_name}', file=sys.stderr)
            continue
        if os.path.isfile(dest) and filecmp.cmp(src, dest, shallow=False):
            continue
        subs += 1
        changed.append(f'web/{dest_rel}  [brand: <- images/{src_name}]')
        if not check:
            shutil.copyfile(src, dest)

    # Raster homepage tiles.
    try:
        from PIL import Image
    except ImportError:
        print('  NOTE: Pillow not installed -- skipping PNG tiles '
              '(pip install Pillow, then re-run)', file=sys.stderr)
    else:
        for rel, swaps in PNG_TARGETS.items():
            path = os.path.join(WEB, *rel.split('/'))
            if not os.path.isfile(path):
                print(f'  WARNING: PNG target missing: {rel}', file=sys.stderr)
                continue
            im = Image.open(path).convert('RGBA')
            px = list(im.getdata())
            table = {src: dst for src, dst in swaps}
            out = [(*table[(r, g, b)], a) if (r, g, b) in table else (r, g, b, a)
                   for r, g, b, a in px]
            if out != px:
                subs += 1
                changed.append(rel)
                if not check:
                    im.putdata(out)
                    im.save(path)

    # Structural patches. Each is idempotent and each fixes something a plain hex swap
    # cannot: a colour pair that carries meaning, or a glyph that would vanish.
    for name, pairs, why in STRUCTURAL:
        path = os.path.join(IMAGES, name)
        if not os.path.isfile(path):
            print(f'  WARNING: structural patch target missing: {name}', file=sys.stderr)
            continue
        text = read(path)
        new = text
        for old, repl in pairs:
            new = re.sub(old, repl, new, flags=re.IGNORECASE)
        if new != text:
            subs += 1
            changed.append(f'{name}  [{why}]')
            if not check:
                write(path, new)

    for c in changed:
        print(f'  {c}')
    verb = 'would change' if check else 'recoloured'
    print(f'\n{verb}: {len(changed)} file(s), {subs} substitution(s)')
    return 1 if (check and changed) else 0


STRUCTURAL = [
    # Upstream ships these byte-identical -- both are a teal square -- so the
    # compatible/incompatible preset indicator currently shows the same icon for both
    # states. Restore the distinction; semantic green/red is out of the grayscale scope.
    ('flag_green.svg', [(r'#009688', '#4CAF50')], 'semantic success green'),
    ('flag_red.svg', [(r'#009688', '#ED1C24')], 'semantic error red'),
    # The lock glyph is #545454 on what was a teal field. Against the strong tier
    # (#3D3D3D) that is 1.2:1 and the icon collapses into a dark blob. Invert the glyph
    # so locked/unlocked differ by POLARITY (dark field/light glyph vs. the reverse),
    # which survives any future palette change.
    ('plate_locked.svg', [(r'#545454', '#FFFFFF')], 'glyph polarity'),
    ('plate_locked_hover.svg', [(r'#545454', '#FFFFFF')], 'glyph polarity'),
    ('plate_locked_dark.svg', [(r'#b3b3b3', '#2D2D31'), (r'#c4c4c4', '#2D2D31')], 'glyph polarity'),
    ('plate_locked_hover_dark.svg', [(r'#b3b3b3', '#2D2D31'), (r'#c4c4c4', '#2D2D31')], 'glyph polarity'),
]


def main():
    args = set(sys.argv[1:])
    if '--audit' in args:
        audit(load_globs('recolor-skip.globs'))
        return 0
    return sweep(check='--check' in args)


if __name__ == '__main__':
    sys.exit(main())
