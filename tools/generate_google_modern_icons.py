#!/usr/bin/env python3
"""Generate modern Google-theme status icons as SVG, PNG previews, and ICO files."""

from __future__ import annotations

import struct
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resource" / "google-modern-icons"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

BLUE = "#3975CE"
CHARCOAL = "#4A4A4A"
BORDER = "#DADCE0"
BLUE_SOFT = "#EAF2FD"
GRAY_SOFT = "#F1F3F4"

ICONS = {
    "zh": ("中", True, "中文"),
    "en": ("A", False, "英文"),
    "full": ("全", True, "全角"),
    "half": ("半", False, "半角"),
}

# SVG text anchoring centers a font's advance box, not the visible glyph. These
# positions compensate for each glyph's real ink bounds after rasterization.
GLYPH_POSITIONS = {
    "中": (128.5, 124.0),
    "A": (129.0, 129.0),
    "全": (129.0, 125.5),
    "半": (128.5, 123.5),
}


def svg_for(style: str, glyph: str, active: bool) -> str:
    accent = BLUE if active else CHARCOAL
    glyph_x, glyph_y = GLYPH_POSITIONS[glyph]
    common = (
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" '
        'viewBox="0 0 256 256">'
    )
    text = (
        f'<text x="{glyph_x}" y="{glyph_y}" text-anchor="middle" '
        'dominant-baseline="middle" '
        'font-family="Segoe UI, Microsoft YaHei UI, Microsoft YaHei, Arial, sans-serif" '
        f'font-size="128" font-weight="700" fill="{{fill}}">{glyph}</text>'
    )

    if style == "outline":
        body = (
            '<rect x="18" y="18" width="220" height="220" rx="48" fill="#FFFFFF" '
            f'stroke="{BORDER}" stroke-width="10"/>'
            f'<path d="M66 31H190" stroke="{accent}" stroke-width="14" '
            'stroke-linecap="round"/>'
            + text.format(fill=accent)
        )
    elif style == "filled":
        body = (
            f'<rect x="18" y="18" width="220" height="220" rx="52" fill="{accent}"/>'
            '<path d="M70 218H186" stroke="#FFFFFF" stroke-opacity="0.34" '
            'stroke-width="8" stroke-linecap="round"/>'
            + text.format(fill="#FFFFFF")
        )
    elif style == "soft":
        bg = BLUE_SOFT if active else GRAY_SOFT
        body = (
            f'<rect x="18" y="18" width="220" height="220" rx="56" fill="{bg}"/>'
            f'<rect x="27" y="27" width="202" height="202" rx="47" fill="none" '
            f'stroke="{accent}" stroke-opacity="0.26" stroke-width="6"/>'
            f'<circle cx="205" cy="51" r="13" fill="{accent}"/>'
            + text.format(fill=accent)
        )
    else:
        raise ValueError(style)

    return common + body + "</svg>\n"


def render_svg(source: Path, target: Path, width: int, height: int | None = None) -> None:
    runtime = (
        Path.home()
        / ".cache/codex-runtimes/codex-primary-runtime/dependencies"
    )
    node = runtime / "node/bin/node"
    sharp = runtime / "node/node_modules/sharp/dist/index.cjs"
    if not node.exists() or not sharp.exists():
        raise RuntimeError("Bundled Codex Node.js + sharp runtime was not found")
    script = (
        "const sharp=require(process.argv[1]);"
        "sharp(process.argv[2]).resize(Number(process.argv[4]),Number(process.argv[5]),{fit:'fill'})"
        ".png().toFile(process.argv[3]);"
    )
    height = width if height is None else height
    subprocess.run(
        [
            str(node),
            "-e",
            script,
            str(sharp),
            str(source),
            str(target),
            str(width),
            str(height),
        ],
        check=True,
    )


def write_ico(pngs: list[Path], target: Path) -> None:
    payloads = [path.read_bytes() for path in pngs]
    header_size = 6 + 16 * len(payloads)
    offset = header_size
    entries = []
    for size, payload in zip(SIZES, payloads, strict=True):
        side = 0 if size == 256 else size
        entries.append(
            struct.pack(
                "<BBBBHHII", side, side, 0, 0, 1, 32, len(payload), offset
            )
        )
        offset += len(payload)
    target.write_bytes(
        struct.pack("<HHH", 0, 1, len(payloads)) + b"".join(entries) + b"".join(payloads)
    )


def preview_svg() -> str:
    width, height = 1120, 980
    cells = []
    styles = (("outline", "商务线框"), ("filled", "商务实心"), ("soft", "柔和轻量"))
    for row, (style, style_label) in enumerate(styles):
        y = 190 + row * 260
        cells.append(
            f'<text x="60" y="{y + 104}" font-family="Arial, sans-serif" '
            f'font-size="28" font-weight="700" fill="#202124">{style_label}</text>'
        )
        for col, (name, (_, _, label)) in enumerate(ICONS.items()):
            x = 280 + col * 200
            cells.append(
                f'<image x="{x}" y="{y}" width="144" height="144" '
                f'href="{style}/{name}.svg"/>'
            )
            cells.append(
                f'<text x="{x + 72}" y="{y + 184}" text-anchor="middle" '
                'font-family="Arial, Microsoft YaHei, sans-serif" font-size="24" '
                f'fill="#5F6368">{label}</text>'
            )
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">'
        '<rect width="100%" height="100%" fill="#FFFFFF"/>'
        '<text x="60" y="72" font-family="Arial, Microsoft YaHei, sans-serif" '
        'font-size="38" font-weight="700" fill="#202124">谷歌皮肤 · 现代商务状态图标</text>'
        '<text x="60" y="116" font-family="Arial, Microsoft YaHei, sans-serif" '
        'font-size="22" fill="#5F6368">Google 蓝 #3975CE · 清晰适配 16–256 px</text>'
        + "".join(cells)
        + "</svg>\n"
    )


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for style in ("outline", "filled", "soft"):
        style_dir = OUT / style
        png_dir = style_dir / "png"
        png_dir.mkdir(parents=True, exist_ok=True)
        for name, (glyph, active, _) in ICONS.items():
            svg = style_dir / f"{name}.svg"
            svg.write_text(svg_for(style, glyph, active), encoding="utf-8")
            rendered = []
            for size in SIZES:
                png = png_dir / f"{name}-{size}.png"
                render_svg(svg, png, size)
                rendered.append(png)
            write_ico(rendered, style_dir / f"{name}.ico")

    preview = OUT / "preview.svg"
    preview.write_text(preview_svg(), encoding="utf-8")
    render_svg(preview, OUT / "preview.png", 1120, 980)
    print(OUT)


if __name__ == "__main__":
    main()
