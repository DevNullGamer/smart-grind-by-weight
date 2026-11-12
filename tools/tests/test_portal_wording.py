"""Pins the user-facing sentences of the web panel that the docs refer to (chg021 V8).

The README states table and docs/PORTAL-API.md quote these sentences, so a redesign must not change them. The
checks run on the built page (web/portal/index.html, what the firmware embeds) and on the sources, so a stale build
is caught by the build check and a changed source string by these tests.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
PORTAL = ROOT / "web" / "portal"
PAGE = (PORTAL / "index.html").read_text(encoding="utf-8")
SOURCE = (PORTAL / "page.src.html").read_text(encoding="utf-8")
VIEW = (PORTAL / "portal-view.js").read_text(encoding="utf-8")

FOOTER = (
    "Anyone on this network can use this page. There is no login. "
    "Turn Web interface off on the grinder when you do not need it."
)
WEB_OFF_HEADING = "The web interface is off"
WEB_OFF_TEXT = (
    "The web interface is switched off on the grinder. Turn it on: Menu, WiFi, Web interface. "
    "This page reconnects by itself."
)
WEB_OFF_BANNER = "The web interface is switched off on the grinder. Turn it on: Menu, WiFi, Web interface."


def test_footer_notice_is_verbatim_in_page_and_source():
    assert FOOTER in SOURCE
    assert FOOTER in PAGE


def test_web_off_card_sentences_are_verbatim_in_page_and_source():
    for text in (WEB_OFF_HEADING, WEB_OFF_TEXT):
        assert text in SOURCE, text
        assert text in PAGE, text


def test_web_off_connection_detail_is_verbatim_in_view():
    assert WEB_OFF_BANNER in VIEW


def test_retry_button_keeps_its_label_and_id():
    assert '<button id="retry" class="btn" type="button" hidden>Retry now</button>' in SOURCE
    assert 'id="retry"' in PAGE and "Retry now" in PAGE


def test_offline_chip_label_is_offline():
    assert "label: 'Offline'" in VIEW
