#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Structural checks for the Ambience QML.

The car's own Qt 5.4 qmllint is the real gate (docs/07) and is not available
on a host, so these are the cheap invariants that catch the two bugs the
overlay checklist exists for: geometry/foreground ownership, the input shield,
and the session-bus Home/Star close path.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
QML = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
PAGE = QML / "HomeScreen/AmbientPage.qml"
TILE = QML / "component/AmbientTile.qml"
SLIDER = QML / "component/AmbientSlider.qml"


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


class OverlayContractTests(unittest.TestCase):
    def setUp(self):
        self.page = text(PAGE)

    def test_spdx_header(self):
        for path in (PAGE, TILE, SLIDER):
            body = text(path)
            self.assertIn("SPDX-FileCopyrightText: 2026 The SCOURGE Project", body)
            self.assertIn("SPDX-License-Identifier: AGPL-3.0-only", body)

    def test_overlay_geometry(self):
        # The page fills the 630 px content band; its instance snippet places
        # it at y = MENU_BAR_Y (checked in InjectionSnippetTests).
        self.assertIn("height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y", self.page)
        self.assertIn("width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH",
                      self.page)
        self.assertIn("height: 630", self.page)

    def test_foreground_gate(self):
        self.assertIn("visible: requested && !atOtherApp", self.page)
        self.assertIn("property bool requested: false", self.page)
        self.assertIn("AppHomeModel.isAtOtherApp", self.page)
        self.assertIn("AppHomeModel.sysPopupShowing", self.page)

    def test_closes_on_navigation_and_home_key(self):
        for hook in ("onButtonBackClicked", "onIsAtOtherAppChanged",
                     "onCurrentScreenChanged",
                     "onCurrentHomeAdvancedPageIdxChanged",
                     "onRequestHomeByPPChanged"):
            self.assertIn(hook, self.page)
        # The page no longer closes on every home_seq bump (a beep or a status
        # write also bumps it): it keeps a baseline and closes on a real change
        # after open, in the status parse site.  Assert that, not the old line.
        self.assertIn("homeSeqBaseline", self.page)
        self.assertIn("_seq > homeSeqBaseline", self.page)

    def test_input_shield(self):
        self.assertIn("preventStealing: true", self.page)
        self.assertIn("acceptedButtons: Qt.AllButtons", self.page)
        self.assertIn("onWheel: wheel.accepted = true", self.page)

    def test_daemon_port_and_routes(self):
        self.assertIn("http://127.0.0.1:8094/status", self.page)
        self.assertIn('"/effect?name="', self.page)
        self.assertIn('"/brightness?value="', self.page)
        self.assertIn('"/transition?ms="', self.page)
        self.assertIn('"/preset?name="', self.page)
        self.assertIn('"/reactive?enabled="', self.page)

    def test_no_hardcoded_theme_colours(self):
        # The page may use status colours (#FFB300) and the dark backdrop, but
        # every text/background colour must come from ConstantModel/ThemeHelper.
        hardcoded = re.findall(r'color:\s*"(#[0-9A-Fa-f]{6})"', self.page)
        # #3A3A3A/#D8D8D8: the back chevron's pressed/ink pair, matching the stock
        # Notifications page (added after this list was written).
        allowed = {"#0E0E0E", "#262626", "#1c1c1c", "#FFB300", "#3A3A3A", "#D8D8D8"}
        self.assertTrue(set(hardcoded) <= allowed, set(hardcoded) - allowed)

    def test_tile_shape(self):
        tile = text(TILE)
        self.assertIn("signal clicked()", tile)
        self.assertIn("EVT_HOME_SOUND_BEEP_PLAY", tile)
        self.assertIn("ambient_icon_n.png", tile)
        self.assertIn("width: 306", tile)
        self.assertIn("height: 249", tile)

    def test_slider_contract(self):
        slider = text(SLIDER)
        self.assertIn("signal committed(real v)", slider)
        self.assertIn("signal moved(real v)", slider)

    def test_braces_balanced(self):
        for path in (PAGE, TILE, SLIDER):
            body = text(path)
            self.assertEqual(body.count("{"), body.count("}"), path.name)
            self.assertEqual(body.count("("), body.count(")"), path.name)


class InjectionSnippetTests(unittest.TestCase):
    def test_tile_instance_uses_the_allocator_and_marker(self):
        snippet = text(ROOT / "scripts/tile.instance.qml")
        self.assertIn("AMBIENT_PAYLOAD_260128", snippet)
        self.assertIn("AMBIENT_TILE_V2", snippet)
        # One instance per grid page, placed by the same allocator the battery/wifi
        # tiles use; the installer rewrites the page, menu id and ordinal.
        self.assertIn("root.firstEmptySlot(@ORD@)", snippet)
        self.assertIn("slot.page === @PAGE0@", snippet)
        self.assertIn("id: ambientTile@PAGE@", snippet)
        self.assertNotIn("menuList", snippet)
        self.assertNotIn("firstEmptyIndex", snippet)
        self.assertIn("onClicked: ambientPage.open()", snippet)

    def test_page_instance_geometry(self):
        snippet = text(ROOT / "scripts/page.instance.qml")
        self.assertIn("AMBIENT_PAYLOAD_260128", snippet)
        self.assertIn("y: ConstantModel.MENU_BAR_Y", snippet)
        self.assertIn("z: 2000", snippet)
        self.assertIn("onGoHome: close()", snippet)

    def test_snippets_carry_no_imports(self):
        for name in ("tile.instance.qml", "page.instance.qml"):
            body = text(ROOT / "scripts" / name)
            self.assertNotIn("\nimport ", body, name)


if __name__ == "__main__":
    unittest.main(verbosity=2)
