#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCHER_PATH = ROOT / "tools" / "apply_cefbrowser_zdf_controls_fix.py"

spec = importlib.util.spec_from_file_location("zdf_static_fix", PATCHER_PATH)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

VIDEO_QUIRKS = """function _quirk_unhide_element(id, clazz, tagName) {
    _quirk_remove_class(id, clazz, tagName, 'quirk_hide_element');
}

function activate_quirks(isStart) {
    if (false) {
    } else if (document.location.href.search("-hbbtv.zdf.de") > 0) {
        if (isStart) {
            _quirk_hide_element('root', null, null);
            document.body.style.background = 'transparent';
            document.getElementsByTagName('html')[0].style.setProperty('background', 'transparent');
        } else {
            _quirk_unhide_element('root', null, null);
            document.body.style.background = '#0d1118';
            document.getElementsByTagName('html')[0].style.setProperty('background', '#0d1118');
        }
    }
}
"""

KEYHANDLER = """(() => {
    window.cefKeyPress = function(keyCode) {
        // create and dispatch the event (keydown)
        let keyDownEvent = new KeyboardEvent("keydown", {bubbles: true});
        document.dispatchEvent(keyDownEvent);

        let keypressEvent = new KeyboardEvent("keypress", {bubbles: true});
        document.dispatchEvent(keypressEvent);

        let keyUpEvent = new KeyboardEvent("keyup", {bubbles: true});
        document.dispatchEvent(keyUpEvent);
    }
})();
"""

CSS = """.quirk_hide_element, .quirk_hide_element * {
    visibility: hidden !important;
}

.quirk_background_transparent {
    background: transparent !important;
}
"""


def write_fixture(root: Path) -> None:
    (root / "js").mkdir(parents=True)
    (root / "css").mkdir(parents=True)
    (root / "js" / "video_quirks.js").write_text(VIDEO_QUIRKS, encoding="utf-8")
    (root / "js" / "keyhandler.js").write_text(KEYHANDLER, encoding="utf-8")
    (root / "css" / "videoquirks.css").write_text(CSS, encoding="utf-8")


with tempfile.TemporaryDirectory(prefix="vdrsuite-zdf-static-") as directory:
    root = Path(directory)
    write_fixture(root)

    assert module.apply(root) == 0
    assert module.check(root) == 0

    video = (root / "js" / "video_quirks.js").read_text(encoding="utf-8")
    keys = (root / "js" / "keyhandler.js").read_text(encoding="utf-8")
    css = (root / "css" / "videoquirks.css").read_text(encoding="utf-8")

    assert "function _quirk_zdf_video_surface(isStart)" in video
    assert "_quirk_zdf_video_surface(true);" in video
    assert "_quirk_zdf_video_surface(false);" in video
    assert "_quirk_hide_element('root', null, null);" not in video

    assert "let keyTarget = document;" in keys
    assert "document.getElementById('root') || document" in keys
    assert keys.count("keyTarget.dispatchEvent(") == 3
    assert "document.dispatchEvent(" not in keys

    assert ".quirk_zdf_video_surface" in css
    assert ".quirk_zdf_video_ancestor" in css

    first = {
        path: path.read_text(encoding="utf-8")
        for path in (
            root / "js" / "video_quirks.js",
            root / "js" / "keyhandler.js",
            root / "css" / "videoquirks.css",
        )
    }

    assert module.apply(root) == 0

    for path, content in first.items():
        assert path.read_text(encoding="utf-8") == content

    assert module.restore(root) == 0
    assert (root / "js" / "video_quirks.js").read_text(encoding="utf-8") == VIDEO_QUIRKS
    assert (root / "js" / "keyhandler.js").read_text(encoding="utf-8") == KEYHANDLER
    assert (root / "css" / "videoquirks.css").read_text(encoding="utf-8") == CSS

print("cefbrowser ZDF static override: patch, idempotency, verification and rollback ok")
