#!/usr/bin/env python3
"""Oracle-backed OpenMoji layout/display lookup and fallback checks."""

import json
import pathlib
import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE = ROOT / "tests" / "oracle.py"
CSS = "html {display:block} body {display:block} p {display:block}"


def png(width, height, rgba):
    rows = b"".join(b"\0" + bytes(rgba) * width for _ in range(height))
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                        8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def oversized_png_header(width, height):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                        8, 6, 0, 0, 0)) +
            chunk(b"IEND", b""))


def run(cwd, command, *args):
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True,
                          check=True, *args).stdout


def walk_layout(node):
    yield node
    for child in node.get("children", []):
        yield from walk_layout(child)


def images(commands):
    output = []
    for command in commands:
        if command["kind"] in ("DrawImage", "image"):
            output.append(command)
        output.extend(images(command.get("children", [])))
    return output


def main():
    native = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        cwd = pathlib.Path(directory)
        assets = cwd / "openmoji"
        assets.mkdir()
        # 4:1 scales to 22:5.5; Python round is ties-to-even => 6.
        (assets / "1F600_color.png").write_bytes(png(4, 1, (255, 0, 0, 128)))
        html = ("<p>😀 next</p><p>😀😀 stays text</p>"
                "<img src='ignored.png'><p>missing: ☃</p>")
        isolated = pathlib.Path(directory) / "oracle"
        isolated.mkdir()
        oracle_reference = isolated / "reference"
        shutil.copytree(ROOT / "tests" / "reference", oracle_reference)
        reference_asset_dir = oracle_reference / "openmoji"
        reference_asset_dir.mkdir()
        reference_asset = reference_asset_dir / "1F600_color.png"
        reference_asset.write_bytes((assets / "1F600_color.png").read_bytes())
        spec = importlib.util.spec_from_file_location("isolated_oracle", ORACLE)
        oracle_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(oracle_module)
        oracle_module.REFERENCE = oracle_reference
        import os
        previous = pathlib.Path.cwd()
        try:
            os.chdir(oracle_reference)
            args = SimpleNamespace(command="layout", css=CSS, rtl=False,
                                   width=800, resolve=None)
            oracle = oracle_module.run(oracle_module.load_reference(), args, html)
        finally:
            os.chdir(previous)
        source = cwd / "case.html"
        source.write_text(html, encoding="utf-8")
        native_page = json.loads(run(cwd, [str(native), source.as_uri()]))

        oracle_emoji = [item for item in walk_layout(oracle["layout"])
                        if item["kind"] == "EmojiLayout"]
        native_emoji = [item for item in walk_layout(native_page["layout"])
                        if item["kind"] == "EmojiLayout"]
        assert len(oracle_emoji) == len(native_emoji) == 1, (
            oracle_emoji, native_emoji, native_page["layout"])
        for key in ("width", "height", "ascent", "descent", "space_after"):
            native_key = "space" if key == "space_after" else key
            assert native_emoji[0][native_key] == oracle_emoji[0][key], (key,
                native_emoji[0], oracle_emoji[0])

        oracle_images = images(oracle["display"])
        native_images = images(native_page["display"])
        assert len(oracle_images) == len(native_images) == 1, (
            oracle_images, native_images, native_page["display"])
        left, top, right, bottom = oracle_images[0]["rect"]
        expected = {"x": left, "y": top, "width": right - left,
                    "height": bottom - top, "image": {"width": 22,
                    "height": 6}}
        actual = {key: native_images[0][key] for key in expected}
        assert actual["image"] == expected["image"]
        for key in ("x", "y", "width", "height"):
            assert abs(actual[key] - expected[key]) < 1e-4, (actual, expected)

        # This deterministic corrupt candidate raises in Skia, so the plain
        # candidate is not attempted and both implementations fall back.
        (assets / "2603_color.png").write_bytes(b"not a png")
        (assets / "2603.png").write_bytes(png(1, 1, (0, 255, 0, 255)))
        (reference_asset_dir / "2603_color.png").write_bytes(b"not a png")
        (reference_asset_dir / "2603.png").write_bytes(
            png(1, 1, (0, 255, 0, 255)))
        corrupt = "<p>☃</p>"
        corrupt_file = cwd / "corrupt.html"
        corrupt_file.write_text(corrupt, encoding="utf-8")
        corrupt_native = json.loads(run(cwd, [str(native),
                                               corrupt_file.as_uri()]))
        previous = pathlib.Path.cwd()
        try:
            os.chdir(oracle_reference)
            corrupt_oracle = oracle_module.run(
                oracle_module.load_reference(), args, corrupt)
        finally:
            os.chdir(previous)
        assert not images(corrupt_native["display"])
        assert not images(corrupt_oracle["display"])

        # Native's documented predecode resource boundary rejects the IHDR
        # before Cairo can allocate the declared surface.
        (assets / "26A0_color.png").write_bytes(
            oversized_png_header(16385, 1))
        limited = cwd / "limited.html"
        limited.write_text("<p>⚠</p>", encoding="utf-8")
        limited_native = json.loads(run(cwd, [str(native), limited.as_uri()]))
        assert not images(limited_native["display"])
        assert any(command.get("text") == "⚠"
                   for command in limited_native["display"])

    print("image differential passed")


if __name__ == "__main__":
    main()
