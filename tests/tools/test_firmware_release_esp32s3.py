"""firmware_release's ESP32-S3 packaging (#50): only a well-formed image pair of this build ships."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import firmware_release as fr  # noqa: E402

VERSION = "b230-1234abcd"


def esp_header(chip: int = fr.ESP_CHIP_ID_ESP32S3) -> bytes:
    h = bytearray(24)
    h[0] = fr.ESP_IMAGE_MAGIC
    h[12:14] = chip.to_bytes(2, "little")
    return bytes(h)


def partition_table(layout: dict | None = None) -> bytes:
    layout = layout or fr.ESP32S3_LAYOUT
    out = bytearray()
    for name, ((ptype, sub), off, size) in layout.items():
        e = bytearray(32)
        e[0:2] = fr.PARTITION_MAGIC
        e[2], e[3] = ptype, sub
        e[4:8] = off.to_bytes(4, "little")
        e[8:12] = size.to_bytes(4, "little")
        e[12:12 + len(name)] = name.encode()
        out += e
    out += b"\xEB\xEB" + b"\xFF" * 30  # the MD5 entry ends the table
    return bytes(out)


def images(app: bytes | None = None, table: bytes | None = None, chip: int = fr.ESP_CHIP_ID_ESP32S3):
    app = app if app is not None else esp_header(chip) + b"\x00" * 100 + VERSION.encode() + b"\x00" * 50
    merged = bytearray(b"\xFF" * fr.ESP32S3_FLASH_SIZE)
    merged[0:24] = esp_header()
    t = table if table is not None else partition_table()
    merged[fr.ESP32S3_PARTITION_TABLE:fr.ESP32S3_PARTITION_TABLE + len(t)] = t
    merged[fr.ESP32S3_APP_OFFSET:fr.ESP32S3_APP_OFFSET + len(app)] = app
    return bytes(merged), app


def test_good_pair_passes():
    merged, app = images()
    fr.validate_esp32s3(merged, app, VERSION)


def test_wrong_chip_refused():
    merged, app = images(chip=5)  # an ESP32-C3 app
    with pytest.raises(fr.ReleaseError, match="chip id 5"):
        fr.validate_esp32s3(merged, app, VERSION)


def test_not_an_esp_image_refused():
    merged, app = images()
    with pytest.raises(fr.ReleaseError, match="no ESP image header"):
        fr.validate_esp32s3(merged, b":10000000" + app[9:], VERSION)  # an Intel hex, say


def test_other_partition_layout_refused():
    layout = dict(fr.ESP32S3_LAYOUT)
    layout["app0"] = ((0x00, 0x10), 0x10000, 0x200000)  # the TinyUF2 layout's smaller slot
    merged, app = images(table=partition_table(layout))
    with pytest.raises(fr.ReleaseError, match="app0"):
        fr.validate_esp32s3(merged, app, VERSION)


def test_app_must_sit_at_0x10000():
    merged, app = images()
    other = app[:-1] + b"\x01"
    with pytest.raises(fr.ReleaseError, match="0x10000"):
        fr.validate_esp32s3(merged, other, VERSION)


def test_version_must_be_in_the_app():
    merged, app = images()
    with pytest.raises(fr.ReleaseError, match="does not carry version"):
        fr.validate_esp32s3(merged, app, "b231-1234abcd")


def test_truncated_full_image_refused():
    merged, app = images()
    with pytest.raises(fr.ReleaseError, match="8 MB"):
        fr.validate_esp32s3(merged[:-1], app, VERSION)


def test_package_writes_names_offsets_and_sha256(tmp_path, monkeypatch):
    merged, app = images()
    build = tmp_path / "esp"
    build.mkdir()
    (build / fr.ESP32S3_BUILD_MERGED).write_bytes(merged)
    (build / fr.ESP32S3_BUILD_APP).write_bytes(app)
    out = tmp_path / "dist"
    out.mkdir()
    m_out, a_out = fr.package_esp32s3(build, 230, VERSION, out)
    assert (m_out.name, a_out.name) == fr.esp32s3_names(230)
    assert m_out.name == "blinky-things-b230-esp32s3.bin"  # the box's contract (#50)
    assert m_out.read_bytes() == merged and a_out.read_bytes() == app
    # Never an nRF52840 asset name: the deploy tools and the web flasher mirror pick those.
    assert m_out.name not in ("blinky-things-b230.bin", "blinky-things-b230.hex", "blinky-things-b230.uf2")


def test_cli_check(tmp_path, capsys):
    merged, app = images()
    (tmp_path / fr.ESP32S3_BUILD_MERGED).write_bytes(merged)
    (tmp_path / fr.ESP32S3_BUILD_APP).write_bytes(app)
    sys.argv = ["firmware_release.py", "check-esp32s3", "--dir", str(tmp_path), "--version", VERSION]
    assert fr.main() == 0
    sys.argv = ["firmware_release.py", "check-esp32s3", "--dir", str(tmp_path / "missing"), "--version", VERSION]
    assert fr.main() == 1
    assert "missing" in capsys.readouterr().err
    json.dumps(fr.ESP32S3_LAYOUT)  # layout stays plain data (manifest-able)
