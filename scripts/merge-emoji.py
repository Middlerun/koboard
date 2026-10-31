#!/usr/bin/env python3
"""Merge weather symbols from a Noto Emoji subset into a DejaVu font.

FBInk's fbink_add_ot_font() *replaces* the font for a given style rather than
appending a fallback, so the weather glyphs have to be baked into the DejaVu
file itself. This copies them out of a pre-subset Noto Emoji font and overrides
DejaVu's own glyphs for those codepoints.

Requires fonttools (pip install fonttools).

Usage:
  merge-emoji.py <dejavu.ttf> <noto-emoji-subset.ttf> <output.ttf>
"""

import sys

from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

# The symbols Koboard offers servers beyond DejaVu's own coverage, taken from
# Noto Emoji rather than DejaVu. Weather, because a forecast is one of the most
# common things a dashboard shows. Change this and the list in the README's
# "Fonts" section together: server authors go by that list.
EMOJI_CODEPOINTS = {
    0x2600,   # sun
    0x2601,   # cloud
    0x2602,   # umbrella
    0x2614,   # umbrella with rain drops
    0x26A1,   # high voltage sign
    0x26C5,   # sun behind cloud
    0x26C8,   # thunder cloud and rain
    0x2744,   # snowflake
    0x1F324,  # sun behind small cloud
    0x1F325,  # sun behind large cloud
    0x1F326,  # sun behind rain cloud
    0x1F327,  # cloud with rain
    0x1F328,  # cloud with snow
    0x1F329,  # cloud with lightning
    0x1F32A,  # cloud with tornado
    0x1F32B,  # fog
    0x1F4A7,  # droplet
}


def mappings(cmap_tbl):
    """The codepoint -> glyph dicts of every cmap subtable that has one.

    Not every subtable kind is a plain mapping (format 14, for variation
    sequences, is not), so those are left out rather than assumed.
    """
    return [sub.cmap for sub in cmap_tbl.tables
            if isinstance(getattr(sub, "cmap", None), dict)]


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 1

    dejavu_path, noto_path, out_path = argv[1:4]

    dv = TTFont(dejavu_path)
    no = TTFont(noto_path)

    d_glyf = dv["glyf"]
    n_cmap = no.getBestCmap()
    n_gset = no.getGlyphSet()
    d_gset = dv.getGlyphSet()
    d_order = dv.getGlyphOrder()
    d_hmtx = dv["hmtx"]
    n_hmtx = no["hmtx"]
    cmap_tbl = dv["cmap"]

    # Locate the format-4 subtable (BMP) and make sure a format-12 one exists
    # for the SMP codepoints (U+1Fxxx need a 32-bit cmap).
    fmt4 = None
    for sub in cmap_tbl.tables:
        if sub.format == 4 and sub.platformID == 3 and sub.platEncID == 1:
            fmt4 = sub
            break

    fmt12 = None
    for sub in cmap_tbl.tables:
        if sub.format == 12:
            fmt12 = sub
            break
    if fmt12 is None:
        fmt12 = CmapSubtable.newSubtable(12)
        fmt12.platformID = 3
        fmt12.platEncID = 10
        fmt12.language = 0
        fmt12.cmap = {}
        cmap_tbl.tables.append(fmt12)

    imported = 0
    for cp in sorted(EMOJI_CODEPOINTS):
        n_name = n_cmap.get(cp)
        if not n_name or n_name not in n_gset:
            # Leave DejaVu's own mapping alone: if it has the glyph, a symbol
            # Noto lacks still renders rather than vanishing.
            kept = any(cp in m for m in mappings(cmap_tbl))
            print(f"U+{cp:04X}: not in Noto Emoji; "
                  + ("keeping DejaVu's glyph" if kept else "no glyph in either font"),
                  file=sys.stderr)
            continue

        new_name = f"uni{cp:04X}" if cp <= 0xFFFF else f"u{cp:06X}"

        try:
            pen = TTGlyphPen(d_gset)
            n_gset[n_name].draw(pen)
            d_glyf[new_name] = pen.glyph()
        except Exception as exc:
            print(f"skipping U+{cp:05X} ({n_name}): {exc}; keeping DejaVu's mapping",
                  file=sys.stderr)
            continue

        # Only now that Noto's glyph is in hand: drop the codepoint from every
        # subtable that maps it, so the new glyph takes over cleanly.
        for m in mappings(cmap_tbl):
            m.pop(cp, None)

        # Copy the advance width from Noto Emoji. A default width makes the
        # glyph overlap the text that follows it.
        metric = n_hmtx.metrics.get(n_name)
        d_hmtx[new_name] = (metric[0] if metric else 1300, 0)

        if cp <= 0xFFFF and fmt4 is not None:
            fmt4.cmap[cp] = new_name
        fmt12.cmap[cp] = new_name

        if new_name not in d_order:
            d_order.append(new_name)

        imported += 1

    dv.save(out_path)
    print(f"merged {imported} glyphs into {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
