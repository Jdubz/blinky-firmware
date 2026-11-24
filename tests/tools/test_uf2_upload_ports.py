"""uf2_upload.py finds blinky-server's device whichever form of the port either side uses.

The server reports its stable /dev/serial/by-id/... symlink and the tool is usually given the
/dev/ttyACMn it points to. Comparing only the tool's own path missed every server device, so
the release (and its reconnect hold) never happened, and the server reopened the port while
the tool was entering the bootloader (test box, 2026-11-23).
"""

from __future__ import annotations

import io
import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import uf2_upload  # noqa: E402


@pytest.fixture
def ports(tmp_path: Path) -> tuple[str, str]:
    """A stand-in tty node and a by-id style symlink to it."""
    tty = tmp_path / "ttyACM0"
    tty.write_text("")
    by_id = tmp_path / "usb-Seeed_XIAO_nRF52840_Sense_87DE9B58C90AFE2D-if00"
    by_id.symlink_to(tty)
    return str(tty), str(by_id)


def test_same_port_resolves_both_sides(ports: tuple[str, str], tmp_path: Path) -> None:
    tty, by_id = ports
    assert uf2_upload._same_port(by_id, tty)
    assert uf2_upload._same_port(tty, by_id)
    assert uf2_upload._same_port(tty, tty)
    assert not uf2_upload._same_port(by_id, str(tmp_path / "ttyACM1"))
    assert not uf2_upload._same_port("", tty)


class _Server:
    """urlopen as a fake blinky-server: one device on the by-id port; records POSTs."""

    def __init__(self, port: str) -> None:
        self.port = port
        self.posts: list[tuple[str, Any]] = []

    def __call__(self, req: Any, timeout: float = 0) -> io.BytesIO:
        url = req.full_url
        if url.endswith("/api/devices"):
            return io.BytesIO(json.dumps([{"id": "87DE9B58C90AFE2D", "port": self.port}]).encode())
        self.posts.append((url, json.loads(req.data or b"{}")))
        return io.BytesIO(b"{}")


def test_release_matches_the_servers_by_id_port_given_the_tty(
    ports: tuple[str, str], monkeypatch: pytest.MonkeyPatch
) -> None:
    tty, by_id = ports
    server = _Server(by_id)
    monkeypatch.setattr(uf2_upload, "urlopen", server)
    assert uf2_upload._request_server_release(tty, hold_seconds=90)
    assert server.posts == [
        (f"{uf2_upload.BLINKY_SERVER_URL}/api/devices/87DE9B58C90AFE2D/release", {"hold_seconds": 90})
    ]


def test_reconnect_matches_the_servers_by_id_port_given_the_tty(
    ports: tuple[str, str], monkeypatch: pytest.MonkeyPatch
) -> None:
    tty, by_id = ports
    server = _Server(by_id)
    monkeypatch.setattr(uf2_upload, "urlopen", server)
    uf2_upload._request_server_reconnect(tty)
    assert [u for u, _ in server.posts] == [
        f"{uf2_upload.BLINKY_SERVER_URL}/api/devices/87DE9B58C90AFE2D/reconnect"
    ]
