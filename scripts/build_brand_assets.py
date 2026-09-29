#!/usr/bin/env python3
"""Build deterministic ExoAnchor logo assets from the selected Riven Datum mark."""

from __future__ import annotations

import json
import hashlib
import math
import os
from pathlib import Path
from typing import Sequence
import xml.etree.ElementTree as ET

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "brand"

INK = "#111318"
INDIGO = "#5966D8"
INDIGO_LIGHT = "#7480F0"
OFF_WHITE = "#F2F3F0"
WHITE = "#F7F9FC"
DEEP_INDIGO = "#0C1022"

CANVAS = 512

# Canonical geometry traced from the largest standalone mark on the selected
# 11-riven-datum.png concept board.  The source trace is uniformly scaled and
# centered here; its fragment proportions and spacing must not be rearranged for
# individual output sizes.
MASTER_POLYGONS: tuple[tuple[tuple[float, float], ...], ...] = (
    ((287.6, 41.5), (235.2, 59.0), (225.7, 147.7), (274.2, 111.4)),
    ((287.6, 130.2), (236.5, 165.2), (247.3, 267.4), (266.1, 280.9),
     (310.5, 205.6)),
    ((112.8, 227.1), (149.1, 299.7), (206.9, 337.4), (206.9, 283.6),
     (174.6, 237.8)),
    ((404.6, 233.8), (357.5, 237.8), (329.3, 271.5), (350.8, 294.3),
     (383.1, 275.5)),
    ((315.8, 306.4), (297.0, 294.3), (266.1, 297.0), (252.6, 307.8),
     (256.7, 334.7), (280.9, 342.7)),
    ((24.0, 317.2), (92.6, 396.5), (184.0, 401.9), (120.8, 334.7)),
    ((488.0, 318.5), (375.0, 342.7), (319.9, 400.6), (418.1, 397.9)),
    ((139.7, 423.4), (236.5, 470.5), (209.6, 435.5), (182.7, 416.7)),
    ((365.6, 423.4), (322.6, 416.7), (301.1, 428.8), (267.4, 470.5)),
)

NODE: tuple[tuple[float, float], ...] = (
    (254.0, 362.9),
    (278.2, 387.1),
    (254.0, 411.3),
    (229.8, 387.1),
)

# Small and large outputs intentionally share one silhouette. Raster sources
# start at 128 px, while browser-managed favicon frames are derived from it.
SMALL_POLYGONS = MASTER_POLYGONS
SMALL_NODE = NODE

# Custom monoline glyphs avoid runtime font dependencies and keep the SVGs
# deterministic. Each glyph lives in a 100 x 160 coordinate cell.
GLYPHS: dict[str, tuple[tuple[tuple[float, float], ...], ...]] = {
    "E": (
        ((86, 10), (20, 10), (20, 150), (86, 150)),
        ((20, 80), (74, 80)),
    ),
    "X": (
        ((15, 10), (85, 150)),
        ((85, 10), (15, 150)),
    ),
    "O": (
        ((36, 10), (69, 10), (87, 28), (87, 132), (69, 150),
         (36, 150), (18, 132), (18, 28), (36, 10)),
    ),
    "A": (
        ((15, 150), (50, 10), (85, 150)),
        ((28, 100), (72, 100)),
    ),
    "N": (
        ((18, 150), (18, 10), (84, 150), (84, 10)),
    ),
    "C": (
        ((87, 18), (71, 10), (36, 10), (18, 28), (18, 132),
         (36, 150), (71, 150), (87, 142)),
    ),
    "H": (
        ((18, 10), (18, 150)),
        ((84, 10), (84, 150)),
        ((18, 80), (84, 80)),
    ),
    "R": (
        ((18, 150), (18, 10), (66, 10), (86, 30), (86, 70),
         (66, 90), (18, 90)),
        ((60, 90), (88, 150)),
    ),
}

WORD = "EXOANCHOR"
GLYPH_WIDTH = 100
GLYPH_HEIGHT = 160
TRACKING = 34
WORD_WIDTH = len(WORD) * GLYPH_WIDTH + (len(WORD) - 1) * TRACKING


def readme_banner_font_path() -> Path:
    override = os.environ.get("EXOANCHOR_WORDMARK_FONT")
    candidates = [
        Path(override).expanduser() if override else None,
        Path.home() / "Library/Fonts/HarmonyOS_Sans_Black.ttf",
        Path("/Library/Fonts/HarmonyOS_Sans_Black.ttf"),
        Path("/usr/local/share/fonts/HarmonyOS_Sans_Black.ttf"),
        Path("/usr/share/fonts/HarmonyOS_Sans_Black.ttf"),
    ]
    for candidate in candidates:
        if candidate is not None and candidate.is_file():
            return candidate
    raise RuntimeError(
        "README banner requires HarmonyOS_Sans_Black.ttf. Install it locally "
        "or set EXOANCHOR_WORDMARK_FONT to the font file."
    )


def ensure_dirs() -> None:
    for name in ("svg", "png", "web", "webp", "hardware"):
        directory = OUT / name
        directory.mkdir(parents=True, exist_ok=True)
        # These subdirectories contain generated files only. Remove stale
        # low-resolution exports when the asset matrix changes.
        for child in directory.iterdir():
            if child.is_file():
                child.unlink()


def fmt(value: float) -> str:
    rounded = round(value, 3)
    if rounded == int(rounded):
        return str(int(rounded))
    return f"{rounded:.3f}".rstrip("0").rstrip(".")


def points_attr(points: Sequence[tuple[float, float]]) -> str:
    return " ".join(f"{fmt(x)},{fmt(y)}" for x, y in points)


def svg_document(view_box: str, body: str, width: str | None = None,
                 height: str | None = None, label: str = "ExoAnchor logo") -> str:
    size = ""
    if width is not None:
        size += f' width="{width}"'
    if height is not None:
        size += f' height="{height}"'
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view_box}"'
        f'{size} role="img" aria-label="{label}">\n'
        f"{body}\n"
        "</svg>\n"
    )


def svg_symbol_group(fragment_color: str, node_color: str,
                     small: bool = False, transform: str | None = None) -> str:
    polygons = SMALL_POLYGONS if small else MASTER_POLYGONS
    node = SMALL_NODE if small else NODE
    attrs = f' transform="{transform}"' if transform else ""
    pieces = [f'<g{attrs}>']
    pieces.extend(
        f'  <polygon points="{points_attr(poly)}" fill="{fragment_color}"/>'
        for poly in polygons
    )
    pieces.append(
        f'  <polygon points="{points_attr(node)}" fill="{node_color}"/>'
    )
    pieces.append("</g>")
    return "\n".join(pieces)


def svg_symbol(fragment_color: str, node_color: str, small: bool = False,
               background: str | None = None, rounded: bool = False) -> str:
    body: list[str] = []
    if background:
        if rounded:
            body.append(
                f'<rect width="512" height="512" rx="96" fill="{background}"/>'
            )
        else:
            body.append(f'<rect width="512" height="512" fill="{background}"/>')
    body.append(svg_symbol_group(fragment_color, node_color, small=small))
    return svg_document("0 0 512 512", "\n".join(body))


def glyph_svg_paths(color: str, x: float, y: float, scale: float = 1.0,
                    stroke_width: float = 11.0) -> str:
    body: list[str] = []
    cursor = x
    for char in WORD:
        for polyline in GLYPHS[char]:
            coords = " ".join(
                f"{fmt(cursor + px * scale)},{fmt(y + py * scale)}"
                for px, py in polyline
            )
            body.append(
                f'<polyline points="{coords}" fill="none" stroke="{color}" '
                f'stroke-width="{fmt(stroke_width * scale)}" '
                'stroke-linecap="square" stroke-linejoin="bevel"/>'
            )
        cursor += (GLYPH_WIDTH + TRACKING) * scale
    return "\n".join(body)


def svg_wordmark(color: str) -> str:
    padding = 16
    body = glyph_svg_paths(color, padding, padding)
    width = WORD_WIDTH + 2 * padding
    height = GLYPH_HEIGHT + 2 * padding
    return svg_document(f"0 0 {width} {height}", body)


def svg_lockup(fragment_color: str, node_color: str, word_color: str,
               background: str | None = None) -> str:
    body: list[str] = []
    if background:
        body.append(f'<rect width="1600" height="360" fill="{background}"/>')
    body.append(
        svg_symbol_group(
            fragment_color,
            node_color,
            transform="translate(32 20) scale(.625)",
        )
    )
    body.append(glyph_svg_paths(word_color, 405, 100, scale=1.0, stroke_width=11))
    return svg_document("0 0 1600 360", "\n".join(body))


def svg_stacked(fragment_color: str, node_color: str,
                word_color: str, background: str | None = None) -> str:
    body: list[str] = []
    if background:
        body.append(f'<rect width="1024" height="768" fill="{background}"/>')
    body.append(
        svg_symbol_group(
            fragment_color,
            node_color,
            transform="translate(312 32) scale(.78125)",
        )
    )
    scale = 0.62
    word_x = (1024 - WORD_WIDTH * scale) / 2
    body.append(glyph_svg_paths(word_color, word_x, 590, scale=scale, stroke_width=11))
    return svg_document("0 0 1024 768", "\n".join(body))


def scaled_polygon(points: Sequence[tuple[float, float]], x: float, y: float,
                   size: float) -> list[tuple[float, float]]:
    scale = size / CANVAS
    return [(x + px * scale, y + py * scale) for px, py in points]


def draw_symbol(draw: ImageDraw.ImageDraw, x: float, y: float, size: float,
                fragment_fill: str, node_fill: str, small: bool = False) -> None:
    polygons = SMALL_POLYGONS if small else MASTER_POLYGONS
    node = SMALL_NODE if small else NODE
    for polygon in polygons:
        draw.polygon(scaled_polygon(polygon, x, y, size), fill=fragment_fill)
    draw.polygon(scaled_polygon(node, x, y, size), fill=node_fill)


def draw_wordmark(draw: ImageDraw.ImageDraw, x: float, y: float, width: float,
                  color: str, stroke_width: float | None = None) -> None:
    scale = width / WORD_WIDTH
    line_width = max(1, round((stroke_width or 11) * scale))
    cursor = x
    for char in WORD:
        for polyline in GLYPHS[char]:
            coords = [
                (cursor + px * scale, y + py * scale)
                for px, py in polyline
            ]
            draw.line(coords, fill=color, width=line_width, joint="curve")
        cursor += (GLYPH_WIDTH + TRACKING) * scale


def draw_font_wordmark(draw: ImageDraw.ImageDraw, x: float, center_y: float,
                       max_width: float, max_height: float, color: str,
                       font_path: Path) -> None:
    probe_size = 1000
    probe = ImageFont.truetype(str(font_path), size=probe_size)
    probe_box = draw.textbbox((0, 0), WORD, font=probe)
    probe_width = probe_box[2] - probe_box[0]
    probe_height = probe_box[3] - probe_box[1]
    font_size = max(
        1,
        math.floor(
            probe_size
            * min(max_width / probe_width, max_height / probe_height)
        ),
    )
    font = ImageFont.truetype(str(font_path), size=font_size)
    box = draw.textbbox((0, 0), WORD, font=font)
    text_height = box[3] - box[1]
    draw.text(
        (x - box[0], center_y - text_height / 2 - box[1]),
        WORD,
        font=font,
        fill=color,
    )


def antialiased_image(width: int, height: int, render, scale: int = 4,
                      background=(0, 0, 0, 0)) -> Image.Image:
    image = Image.new("RGBA", (width * scale, height * scale), background)
    draw = ImageDraw.Draw(image)
    render(draw, scale)
    return image.resize((width, height), Image.Resampling.LANCZOS)


def render_symbol_png(size: int, reverse: bool = False,
                      app_background: bool = False, maskable: bool = False,
                      rounded_background: bool = True,
                      small_geometry: bool | None = None) -> Image.Image:
    def render(draw: ImageDraw.ImageDraw, scale: int) -> None:
        side = size * scale
        if app_background:
            if rounded_background:
                radius = int(side * 0.19)
                draw.rounded_rectangle(
                    (0, 0, side, side),
                    radius=radius,
                    fill=DEEP_INDIGO,
                )
            else:
                draw.rectangle((0, 0, side, side), fill=DEEP_INDIGO)
        padding_ratio = 0.22 if maskable else (0.12 if app_background else 0.02)
        pad = side * padding_ratio
        symbol_size = side - 2 * pad
        fragment = WHITE if (reverse or app_background) else INK
        node = INDIGO_LIGHT if (reverse or app_background) else INDIGO
        draw_symbol(
            draw,
            pad,
            pad,
            symbol_size,
            fragment,
            node,
            small=(size <= 48 if small_geometry is None else small_geometry),
        )

    return antialiased_image(size, size, render)


def render_lockup_png(width: int, reverse: bool = False,
                      solid_background: bool = False) -> Image.Image:
    height = max(1, round(width * 0.225))

    def render(draw: ImageDraw.ImageDraw, scale: int) -> None:
        canvas_w = width * scale
        canvas_h = height * scale
        if solid_background:
            draw.rectangle((0, 0, canvas_w, canvas_h), fill=DEEP_INDIGO)
        fragment = WHITE if (reverse or solid_background) else INK
        word = WHITE if (reverse or solid_background) else INK
        node = INDIGO_LIGHT if (reverse or solid_background) else INDIGO
        symbol_size = canvas_h * 0.88
        symbol_x = canvas_h * 0.05
        symbol_y = (canvas_h - symbol_size) / 2
        draw_symbol(
            draw,
            symbol_x,
            symbol_y,
            symbol_size,
            fragment,
            node,
            small=height <= 72,
        )
        word_x = symbol_x + symbol_size + canvas_h * 0.11
        word_width = canvas_w - word_x - canvas_h * 0.08
        word_scale = word_width / WORD_WIDTH
        word_height = GLYPH_HEIGHT * word_scale
        word_y = (canvas_h - word_height) / 2
        draw_wordmark(draw, word_x, word_y, word_width, word)

    return antialiased_image(width, height, render)


def render_readme_banner() -> Image.Image:
    width, height = 1600, 400
    font_path = readme_banner_font_path()

    def render(draw: ImageDraw.ImageDraw, scale: int) -> None:
        w, h = width * scale, height * scale
        draw.rectangle((0, 0, w, h), fill=DEEP_INDIGO)
        symbol_size = h * 0.82
        symbol_x = h * 0.16
        symbol_y = (h - symbol_size) / 2
        draw_symbol(draw, symbol_x, symbol_y, symbol_size, WHITE, INDIGO_LIGHT)
        word_x = symbol_x + symbol_size + h * 0.16
        word_width = w - word_x - h * 0.18
        draw_font_wordmark(
            draw,
            word_x,
            h / 2,
            word_width,
            h * 0.45,
            WHITE,
            font_path,
        )

    return antialiased_image(width, height, render)


def render_social_preview() -> Image.Image:
    width, height = 1280, 640

    def render(draw: ImageDraw.ImageDraw, scale: int) -> None:
        w, h = width * scale, height * scale
        draw.rectangle((0, 0, w, h), fill=DEEP_INDIGO)
        # Subtle IndigoShore horizon rule.
        draw.rectangle(
            (0, int(h * 0.735), w, int(h * 0.742)),
            fill="#222A61",
        )
        symbol_size = h * 0.58
        symbol_x = h * 0.14
        symbol_y = h * 0.16
        draw_symbol(draw, symbol_x, symbol_y, symbol_size, WHITE, INDIGO_LIGHT)
        word_x = symbol_x + symbol_size + h * 0.12
        word_width = w - word_x - h * 0.12
        word_scale = word_width / WORD_WIDTH
        word_y = h * 0.39
        draw_wordmark(draw, word_x, word_y, word_width, WHITE)

    return antialiased_image(width, height, render)


def render_preview_sheet() -> Image.Image:
    width, height = 1600, 1100

    def render(draw: ImageDraw.ImageDraw, scale: int) -> None:
        w, h = width * scale, height * scale
        draw.rectangle((0, 0, w, h), fill=OFF_WHITE)
        draw.rectangle((0, int(h * 0.58), w, h), fill=DEEP_INDIGO)
        draw_symbol(draw, 90 * scale, 80 * scale, 360 * scale, INK, INDIGO)
        draw_wordmark(draw, 500 * scale, 170 * scale, 980 * scale, INK)
        draw_symbol(draw, 100 * scale, 680 * scale, 290 * scale, WHITE, INDIGO_LIGHT)
        draw_wordmark(draw, 450 * scale, 735 * scale, 960 * scale, WHITE)
        sizes = (24, 32, 48, 64, 96)
        cursor = 970
        for size in sizes:
            box = 54 + size
            draw.rounded_rectangle(
                (cursor * scale, 500 * scale,
                 (cursor + box) * scale, (500 + box) * scale),
                radius=round(10 * scale),
                fill=DEEP_INDIGO,
            )
            pad = box * 0.14
            draw_symbol(
                draw,
                (cursor + pad) * scale,
                (500 + pad) * scale,
                (box - 2 * pad) * scale,
                WHITE,
                INDIGO_LIGHT,
                small=size <= 48,
            )
            cursor += box + 20

    return antialiased_image(width, height, render, scale=2)


def dxf_polyline(points: Sequence[tuple[float, float]], scale: float,
                 origin_x: float, origin_y: float, layer: str) -> str:
    rows = [
        "0", "LWPOLYLINE",
        "8", layer,
        "90", str(len(points)),
        "70", "1",
    ]
    for x, y in points:
        rows.extend(("10", fmt(origin_x + x * scale), "20", fmt(origin_y - y * scale)))
    return "\n".join(rows) + "\n"


def write_dxf(path: Path, width_mm: float) -> None:
    all_points = [point for polygon in MASTER_POLYGONS for point in polygon] + list(NODE)
    min_x = min(x for x, _ in all_points)
    max_x = max(x for x, _ in all_points)
    min_y = min(y for _, y in all_points)
    max_y = max(y for _, y in all_points)
    scale = width_mm / (max_x - min_x)
    height_mm = (max_y - min_y) * scale
    origin_x = -min_x * scale
    origin_y = max_y * scale
    entities = "".join(
        dxf_polyline(poly, scale, origin_x, origin_y, "EXOANCHOR_SILK")
        for poly in (*MASTER_POLYGONS, NODE)
    )
    content = (
        "0\nSECTION\n2\nHEADER\n"
        "9\n$INSUNITS\n70\n4\n"
        "9\n$EXTMIN\n10\n0\n20\n0\n"
        f"9\n$EXTMAX\n10\n{fmt(width_mm)}\n20\n{fmt(height_mm)}\n"
        "0\nENDSEC\n"
        "0\nSECTION\n2\nTABLES\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        f"{entities}"
        "0\nENDSEC\n0\nEOF\n"
    )
    path.write_text(content, encoding="ascii")


def write_hardware_svg(path: Path, width_mm: float) -> None:
    all_points = [point for polygon in MASTER_POLYGONS for point in polygon] + list(NODE)
    min_x = min(x for x, _ in all_points)
    max_x = max(x for x, _ in all_points)
    min_y = min(y for _, y in all_points)
    max_y = max(y for _, y in all_points)
    height_mm = width_mm * (max_y - min_y) / (max_x - min_x)
    body = "\n".join(
        f'<polygon points="{points_attr(poly)}" fill="#000000"/>'
        for poly in (*MASTER_POLYGONS, NODE)
    )
    svg = svg_document(
        "0 0 512 512",
        body,
        width=f"{fmt(width_mm)}mm",
        height=f"{fmt(height_mm)}mm",
        label="ExoAnchor silkscreen mark",
    )
    path.write_text(svg, encoding="utf-8")


def save_png(image: Image.Image, path: Path) -> None:
    image.save(path, "PNG", optimize=True)


def build_svg_assets() -> None:
    assets = {
        "exoanchor-symbol-color.svg": svg_symbol(INK, INDIGO),
        "exoanchor-symbol-mono.svg": svg_symbol(INK, INK),
        "exoanchor-symbol-reverse.svg": svg_symbol(WHITE, INDIGO_LIGHT),
        "exoanchor-symbol-small.svg": svg_symbol(INK, INDIGO, small=True),
        "exoanchor-wordmark-mono.svg": svg_wordmark(INK),
        "exoanchor-wordmark-reverse.svg": svg_wordmark(WHITE),
        "exoanchor-lockup-horizontal-color.svg": svg_lockup(INK, INDIGO, INK),
        "exoanchor-lockup-horizontal-mono.svg": svg_lockup(INK, INK, INK),
        "exoanchor-lockup-horizontal-reverse.svg": svg_lockup(
            WHITE, INDIGO_LIGHT, WHITE
        ),
        "exoanchor-lockup-stacked-color.svg": svg_stacked(INK, INDIGO, INK),
        "exoanchor-lockup-stacked-reverse.svg": svg_stacked(
            WHITE, INDIGO_LIGHT, WHITE
        ),
    }
    for name, content in assets.items():
        (OUT / "svg" / name).write_text(content, encoding="utf-8")

    favicon_svg = svg_symbol(
        WHITE,
        INDIGO_LIGHT,
        small=True,
        background=DEEP_INDIGO,
        rounded=True,
    )
    (OUT / "web" / "favicon.svg").write_text(favicon_svg, encoding="utf-8")
    (OUT / "web" / "exoanchor-ui-mark.svg").write_text(
        svg_symbol(WHITE, INDIGO_LIGHT, small=True),
        encoding="utf-8",
    )


def build_raster_assets() -> None:
    for size in (128, 256, 512, 1024):
        save_png(
            render_symbol_png(size),
            OUT / "png" / f"exoanchor-symbol-color-{size}.png",
        )
    for size in (128, 256, 512):
        save_png(
            render_symbol_png(size, reverse=True),
            OUT / "png" / f"exoanchor-symbol-reverse-{size}.png",
        )
    for width in (600, 1200, 2400):
        save_png(
            render_lockup_png(width),
            OUT / "png" / f"exoanchor-lockup-horizontal-color-{width}.png",
        )
    save_png(
        render_lockup_png(1200, reverse=True),
        OUT / "png" / "exoanchor-lockup-horizontal-reverse-1200.png",
    )
    save_png(
        render_readme_banner(),
        OUT / "png" / "exoanchor-readme-banner-1600x400.png",
    )
    save_png(
        render_social_preview(),
        OUT / "png" / "exoanchor-social-preview-1280x640.png",
    )
    save_png(
        render_preview_sheet(),
        OUT / "brand-assets-preview.png",
    )

    for size in (128, 256):
        save_png(
            render_symbol_png(
                size,
                app_background=True,
                small_geometry=True,
            ),
            OUT / "web" / f"ui-mark-{size}.png",
        )

    app_180 = render_symbol_png(
        180, app_background=True, rounded_background=False
    )
    app_192 = render_symbol_png(
        192, app_background=True, rounded_background=False
    )
    app_512 = render_symbol_png(
        512, app_background=True, rounded_background=False
    )
    maskable_512 = render_symbol_png(
        512,
        app_background=True,
        maskable=True,
        rounded_background=False,
    )
    save_png(app_180, OUT / "web" / "apple-touch-icon-180.png")
    save_png(app_192, OUT / "web" / "pwa-icon-192.png")
    save_png(app_512, OUT / "web" / "pwa-icon-512.png")
    save_png(maskable_512, OUT / "web" / "pwa-icon-maskable-512.png")

    favicon_source = render_symbol_png(
        256,
        app_background=True,
        small_geometry=True,
    )
    save_png(favicon_source, OUT / "web" / "favicon-source-256.png")
    favicon_source.save(
        OUT / "web" / "favicon.ico",
        format="ICO",
        sizes=[(16, 16), (32, 32), (48, 48)],
    )

    render_symbol_png(256).save(
        OUT / "webp" / "exoanchor-symbol-color-256.webp",
        "WEBP",
        lossless=True,
        method=6,
    )
    render_lockup_png(1200).save(
        OUT / "webp" / "exoanchor-lockup-horizontal-color-1200.webp",
        "WEBP",
        lossless=True,
        method=6,
    )


def build_hardware_assets() -> None:
    (OUT / "hardware" / "exoanchor-symbol-silkscreen.svg").write_text(
        svg_symbol("#000000", "#000000"),
        encoding="utf-8",
    )
    for size in (10.0, 20.0):
        suffix = str(int(size))
        write_hardware_svg(
            OUT / "hardware" / f"exoanchor-symbol-silkscreen-{suffix}mm.svg",
            size,
        )
        write_dxf(
            OUT / "hardware" / f"exoanchor-symbol-silkscreen-{suffix}mm.dxf",
            size,
        )


def write_manifest() -> None:
    entries = []
    for path in sorted(OUT.rglob("*")):
        if path.is_file() and path.name != "manifest.json":
            item: dict[str, object] = {
                "path": path.relative_to(OUT).as_posix(),
                "bytes": path.stat().st_size,
            }
            if path.suffix.lower() in {".png", ".webp", ".ico"}:
                with Image.open(path) as image:
                    item["format"] = image.format
                    item["size"] = list(image.size)
                    item["mode"] = image.mode
                    if path.suffix.lower() == ".ico":
                        item["frames"] = sorted(
                            [list(size) for size in image.info.get("sizes", [])]
                        )
            entries.append(item)
    manifest = {
        "schema": "exoanchor.brand-assets.v2",
        "selected_direction": "11-riven-datum",
        "readme_banner": {
            "wordmark": WORD,
            "font": "HarmonyOS Sans Black",
            "font_file": readme_banner_font_path().name,
            "font_sha256": hashlib.sha256(
                readme_banner_font_path().read_bytes()
            ).hexdigest(),
        },
        "colors": {
            "ink": INK,
            "indigo": INDIGO,
            "indigo_light": INDIGO_LIGHT,
            "off_white": OFF_WHITE,
            "white": WHITE,
            "deep_indigo": DEEP_INDIGO,
        },
        "assets": entries,
    }
    (OUT / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def orientation(a: tuple[float, float], b: tuple[float, float],
                c: tuple[float, float]) -> float:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def point_on_segment(point: tuple[float, float], a: tuple[float, float],
                     b: tuple[float, float], epsilon: float = 1e-7) -> bool:
    if abs(orientation(a, b, point)) > epsilon:
        return False
    return (
        min(a[0], b[0]) - epsilon <= point[0] <= max(a[0], b[0]) + epsilon
        and min(a[1], b[1]) - epsilon <= point[1] <= max(a[1], b[1]) + epsilon
    )


def segments_intersect(a: tuple[float, float], b: tuple[float, float],
                       c: tuple[float, float], d: tuple[float, float]) -> bool:
    o1 = orientation(a, b, c)
    o2 = orientation(a, b, d)
    o3 = orientation(c, d, a)
    o4 = orientation(c, d, b)
    if ((o1 > 0 > o2) or (o2 > 0 > o1)) and (
        (o3 > 0 > o4) or (o4 > 0 > o3)
    ):
        return True
    return any((
        abs(o1) <= 1e-7 and point_on_segment(c, a, b),
        abs(o2) <= 1e-7 and point_on_segment(d, a, b),
        abs(o3) <= 1e-7 and point_on_segment(a, c, d),
        abs(o4) <= 1e-7 and point_on_segment(b, c, d),
    ))


def point_in_polygon(point: tuple[float, float],
                     polygon: Sequence[tuple[float, float]]) -> bool:
    x, y = point
    inside = False
    j = len(polygon) - 1
    for i, current in enumerate(polygon):
        previous = polygon[j]
        if point_on_segment(point, previous, current):
            return True
        crosses = ((current[1] > y) != (previous[1] > y))
        if crosses:
            boundary_x = (
                (previous[0] - current[0])
                * (y - current[1])
                / (previous[1] - current[1])
                + current[0]
            )
            if x < boundary_x:
                inside = not inside
        j = i
    return inside


def polygon_edges(polygon: Sequence[tuple[float, float]]):
    for index, point in enumerate(polygon):
        yield point, polygon[(index + 1) % len(polygon)]


def polygons_overlap(a: Sequence[tuple[float, float]],
                     b: Sequence[tuple[float, float]]) -> bool:
    if any(
        segments_intersect(a1, a2, b1, b2)
        for a1, a2 in polygon_edges(a)
        for b1, b2 in polygon_edges(b)
    ):
        return True
    return point_in_polygon(a[0], b) or point_in_polygon(b[0], a)


def point_segment_distance(point: tuple[float, float],
                           a: tuple[float, float],
                           b: tuple[float, float]) -> float:
    dx = b[0] - a[0]
    dy = b[1] - a[1]
    length_squared = dx * dx + dy * dy
    if length_squared == 0:
        return math.dist(point, a)
    projection = (
        (point[0] - a[0]) * dx + (point[1] - a[1]) * dy
    ) / length_squared
    projection = max(0.0, min(1.0, projection))
    nearest = (a[0] + projection * dx, a[1] + projection * dy)
    return math.dist(point, nearest)


def polygon_distance(a: Sequence[tuple[float, float]],
                     b: Sequence[tuple[float, float]]) -> float:
    distances = [
        point_segment_distance(point, edge_a, edge_b)
        for point in a
        for edge_a, edge_b in polygon_edges(b)
    ]
    distances.extend(
        point_segment_distance(point, edge_a, edge_b)
        for point in b
        for edge_a, edge_b in polygon_edges(a)
    )
    return min(distances)


def validate_geometry(label: str,
                      polygons: Sequence[Sequence[tuple[float, float]]],
                      node: Sequence[tuple[float, float]],
                      minimum_gap: float = 12.0) -> None:
    shapes = [*polygons, node]
    for index, shape in enumerate(shapes):
        for other_index in range(index + 1, len(shapes)):
            other = shapes[other_index]
            if polygons_overlap(shape, other):
                raise RuntimeError(
                    f"{label} polygons {index} and {other_index} overlap"
                )
            distance = polygon_distance(shape, other)
            if distance < minimum_gap:
                raise RuntimeError(
                    f"{label} polygons {index} and {other_index} have "
                    f"{distance:.2f} units of clearance; expected >= {minimum_gap}"
                )


def validate_assets() -> None:
    required = {
        "svg/exoanchor-symbol-color.svg",
        "svg/exoanchor-symbol-mono.svg",
        "svg/exoanchor-symbol-reverse.svg",
        "svg/exoanchor-symbol-small.svg",
        "svg/exoanchor-lockup-horizontal-color.svg",
        "web/exoanchor-ui-mark.svg",
        "web/ui-mark-128.png",
        "web/favicon.svg",
        "web/favicon.ico",
        "web/favicon-source-256.png",
        "web/apple-touch-icon-180.png",
        "web/pwa-icon-192.png",
        "web/pwa-icon-512.png",
        "web/pwa-icon-maskable-512.png",
        "png/exoanchor-social-preview-1280x640.png",
        "hardware/exoanchor-symbol-silkscreen.svg",
        "hardware/exoanchor-symbol-silkscreen-10mm.dxf",
        "hardware/exoanchor-symbol-silkscreen-20mm.dxf",
        "manifest.json",
    }
    missing = sorted(path for path in required if not (OUT / path).is_file())
    if missing:
        raise RuntimeError(f"Missing brand assets: {', '.join(missing)}")

    for path in OUT.rglob("*.svg"):
        ET.parse(path)

    validate_geometry("master", MASTER_POLYGONS, NODE)
    validate_geometry("small", SMALL_POLYGONS, SMALL_NODE)

    expected_rasters = {
        "web/apple-touch-icon-180.png": (180, 180),
        "web/pwa-icon-192.png": (192, 192),
        "web/pwa-icon-512.png": (512, 512),
        "web/pwa-icon-maskable-512.png": (512, 512),
        "png/exoanchor-social-preview-1280x640.png": (1280, 640),
        "png/exoanchor-readme-banner-1600x400.png": (1600, 400),
    }
    for relative, expected in expected_rasters.items():
        with Image.open(OUT / relative) as image:
            if image.size != expected:
                raise RuntimeError(
                    f"{relative} is {image.size}, expected {expected}"
                )

    with Image.open(OUT / "web" / "favicon.ico") as icon:
        sizes = set(icon.info.get("sizes", set()))
        expected_sizes = {(16, 16), (32, 32), (48, 48)}
        if sizes != expected_sizes:
            raise RuntimeError(
                f"favicon.ico has frames {sorted(sizes)}, "
                f"expected {sorted(expected_sizes)}"
            )

    icon_patterns = (
        "exoanchor-symbol-",
        "ui-mark-",
        "favicon-source-",
        "apple-touch-icon-",
        "pwa-icon-",
    )
    for path in OUT.rglob("*.png"):
        if not path.name.startswith(icon_patterns):
            continue
        with Image.open(path) as image:
            if min(image.size) < 128:
                raise RuntimeError(
                    f"{path.relative_to(OUT)} is below the 128 px icon source minimum"
                )

    for name in ("web/exoanchor-ui-mark.svg", "web/favicon.svg"):
        byte_count = (OUT / name).stat().st_size
        if byte_count > 16 * 1024:
            raise RuntimeError(f"{name} is too large for firmware: {byte_count} bytes")

    for path in OUT.glob("hardware/*.dxf"):
        if not path.read_text(encoding="ascii").rstrip().endswith("EOF"):
            raise RuntimeError(f"{path.name} is not a complete DXF")


def main() -> None:
    ensure_dirs()
    build_svg_assets()
    build_raster_assets()
    build_hardware_assets()
    write_manifest()
    validate_assets()
    asset_count = sum(1 for path in OUT.rglob("*") if path.is_file())
    print(f"Built and validated {asset_count} ExoAnchor brand assets in {OUT}")


if __name__ == "__main__":
    main()
