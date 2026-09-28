#!/usr/bin/env python3
"""Apply the pinned VDR-Suite ZDF HbbTV static-content correction to cefbrowser.

This deliberately modifies only static browser assets. It does not patch the
cefbrowser executable and it refuses unknown source shapes.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys

PINNED_CEFBROWSER_COMMIT = "d14517be4a7aecfc3ba0b77831af237721ef48e9"
BACKUP_SUFFIX = ".vdr-suite-zdf-controls.pre-fix"

VIDEO_QUIRKS = Path("js/video_quirks.js")
KEYHANDLER = Path("js/keyhandler.js")
VIDEOQUIRKS_CSS = Path("css/videoquirks.css")

ZDF_HELPER_MARKER = "function _quirk_zdf_video_surface(isStart)"
KEY_TARGET_MARKER = "let keyTarget = document;"
CSS_VIDEO_MARKER = ".quirk_zdf_video_surface"
CSS_ANCESTOR_MARKER = ".quirk_zdf_video_ancestor"

HELPER_ANCHOR = """function _quirk_unhide_element(id, clazz, tagName) {
    _quirk_remove_class(id, clazz, tagName, 'quirk_hide_element');
}
"""

HELPER_INSERT = """function _quirk_unhide_element(id, clazz, tagName) {
    _quirk_remove_class(id, clazz, tagName, 'quirk_hide_element');
}

function _quirk_zdf_video_surface(isStart) {
    const videos = document.getElementsByTagName('video');

    for (let i = 0; i < videos.length; ++i) {
        const video = videos[i];

        if (isStart) {
            video.classList.add('quirk_zdf_video_surface');
        } else {
            video.classList.remove('quirk_zdf_video_surface');
        }

        let parent = video.parentElement;
        while (parent !== null && parent !== document.body) {
            if (isStart) {
                parent.classList.add('quirk_zdf_video_ancestor');
            } else {
                parent.classList.remove('quirk_zdf_video_ancestor');
            }

            if (parent.id === 'root') {
                break;
            }
            parent = parent.parentElement;
        }
    }
}
"""

ZDF_START_OLD = """        if (isStart) {
            _quirk_hide_element('root', null, null);
            document.body.style.background = 'transparent';
"""
ZDF_START_NEW = """        if (isStart) {
            _quirk_zdf_video_surface(true);
            document.body.style.background = 'transparent';
"""

ZDF_STOP_OLD = """        } else {
            _quirk_unhide_element('root', null, null);
            document.body.style.background = '#0d1118';
"""
ZDF_STOP_NEW = """        } else {
            _quirk_zdf_video_surface(false);
            document.body.style.background = '#0d1118';
"""

KEY_FUNCTION_OLD = """    window.cefKeyPress = function(keyCode) {
        // create and dispatch the event (keydown)
"""
KEY_FUNCTION_NEW = """    window.cefKeyPress = function(keyCode) {
        let keyTarget = document;
        if (document.location.href.search("-hbbtv.zdf.de") > 0) {
            const activeElement = document.activeElement;
            if (activeElement &&
                activeElement !== document.body &&
                activeElement !== document.documentElement) {
                keyTarget = activeElement;
            } else {
                keyTarget = document.getElementById('root') || document;
            }
        }

        // create and dispatch the event (keydown)
"""

CSS_APPEND = """
/*
 * VDR-Suite ZDF external-video composition:
 * keep the HbbTV application/root visible so native controls and focus survive,
 * hide only the browser's video pixels, and make the video ancestor chain
 * transparent so the external media plane can show through.
 */
.quirk_zdf_video_surface {
    visibility: hidden !important;
}

.quirk_zdf_video_ancestor {
    background: transparent !important;
}
"""


class PatchError(RuntimeError):
    pass


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise PatchError(
            f"{label}: expected exactly one source marker, found {count}"
        )
    return text.replace(old, new, 1)


def patched_video_quirks(text: str) -> bool:
    return (
        ZDF_HELPER_MARKER in text
        and "_quirk_zdf_video_surface(true);" in text
        and "_quirk_zdf_video_surface(false);" in text
        and "_quirk_hide_element('root', null, null);" not in text
    )


def patched_keyhandler(text: str) -> bool:
    return (
        KEY_TARGET_MARKER in text
        and text.count("keyTarget.dispatchEvent(") == 3
        and text.count("document.dispatchEvent(") == 0
    )


def patched_css(text: str) -> bool:
    return CSS_VIDEO_MARKER in text and CSS_ANCESTOR_MARKER in text


def patch_video_quirks(text: str) -> str:
    if patched_video_quirks(text):
        return text
    text = replace_once(text, HELPER_ANCHOR, HELPER_INSERT, "video_quirks helper")
    text = replace_once(text, ZDF_START_OLD, ZDF_START_NEW, "ZDF video start")
    text = replace_once(text, ZDF_STOP_OLD, ZDF_STOP_NEW, "ZDF video stop")
    if not patched_video_quirks(text):
        raise PatchError("video_quirks postcondition failed")
    return text


def patch_keyhandler(text: str) -> str:
    if patched_keyhandler(text):
        return text
    text = replace_once(
        text, KEY_FUNCTION_OLD, KEY_FUNCTION_NEW, "keyhandler target setup"
    )
    dispatch_count = text.count("document.dispatchEvent(")
    if dispatch_count != 3:
        raise PatchError(
            f"keyhandler dispatch source count changed: expected 3, found {dispatch_count}"
        )
    text = text.replace("document.dispatchEvent(", "keyTarget.dispatchEvent(")
    if not patched_keyhandler(text):
        raise PatchError("keyhandler postcondition failed")
    return text


def patch_css(text: str) -> str:
    if patched_css(text):
        return text
    text = text.rstrip() + "\n" + CSS_APPEND.lstrip()
    if not patched_css(text):
        raise PatchError("videoquirks.css postcondition failed")
    return text


def target_paths(root: Path) -> dict[Path, callable]:
    return {
        root / VIDEO_QUIRKS: patch_video_quirks,
        root / KEYHANDLER: patch_keyhandler,
        root / VIDEOQUIRKS_CSS: patch_css,
    }


def read_targets(root: Path) -> dict[Path, str]:
    values: dict[Path, str] = {}
    for path in target_paths(root):
        if not path.is_file():
            raise PatchError(f"required cefbrowser static file missing: {path}")
        values[path] = path.read_text(encoding="utf-8")
    return values


def is_fully_patched(values: dict[Path, str], root: Path) -> bool:
    return (
        patched_video_quirks(values[root / VIDEO_QUIRKS])
        and patched_keyhandler(values[root / KEYHANDLER])
        and patched_css(values[root / VIDEOQUIRKS_CSS])
    )


def stage_and_replace(changes: dict[Path, str]) -> None:
    staged: list[tuple[Path, Path]] = []
    try:
        for path, content in changes.items():
            tmp = path.with_name(path.name + ".vdr-suite-zdf-controls.tmp")
            tmp.write_text(content, encoding="utf-8")
            os.chmod(tmp, path.stat().st_mode)
            staged.append((path, tmp))

        for path, _ in staged:
            backup = path.with_name(path.name + BACKUP_SUFFIX)
            if not backup.exists():
                shutil.copy2(path, backup)

        for path, tmp in staged:
            os.replace(tmp, path)
    finally:
        for _, tmp in staged:
            if tmp.exists():
                tmp.unlink()


def apply(root: Path) -> int:
    root = root.resolve()
    original = read_targets(root)

    if is_fully_patched(original, root):
        print(f"CEFBROWSER_ZDF_STATIC_FIX=ALREADY_APPLIED root={root}")
        return 0

    changes: dict[Path, str] = {}
    for path, transformer in target_paths(root).items():
        changes[path] = transformer(original[path])

    if not is_fully_patched(changes, root):
        raise PatchError("combined postcondition failed before write")

    for path in changes:
        print(
            f"PATCH {path}: {digest(original[path])} -> {digest(changes[path])}"
        )

    stage_and_replace(changes)

    verified = read_targets(root)
    if not is_fully_patched(verified, root):
        raise PatchError("post-write verification failed")

    print(
        "CEFBROWSER_ZDF_STATIC_FIX=PASS "
        f"upstream={PINNED_CEFBROWSER_COMMIT} root={root}"
    )
    return 0


def check(root: Path) -> int:
    root = root.resolve()
    values = read_targets(root)
    if not is_fully_patched(values, root):
        raise PatchError("cefbrowser ZDF static fix is not fully applied")
    print(
        "CEFBROWSER_ZDF_STATIC_FIX_CHECK=PASS "
        f"upstream={PINNED_CEFBROWSER_COMMIT} root={root}"
    )
    return 0


def restore(root: Path) -> int:
    root = root.resolve()
    restored = 0
    for path in target_paths(root):
        backup = path.with_name(path.name + BACKUP_SUFFIX)
        if not backup.is_file():
            raise PatchError(f"backup missing: {backup}")
        tmp = path.with_name(path.name + ".vdr-suite-zdf-controls.restore.tmp")
        shutil.copy2(backup, tmp)
        os.replace(tmp, path)
        restored += 1
    print(f"CEFBROWSER_ZDF_STATIC_FIX_RESTORE=PASS files={restored} root={root}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, help="cefbrowser static-content root")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--restore", action="store_true")
    args = parser.parse_args()

    try:
        root = Path(args.root)
        if args.apply:
            return apply(root)
        if args.check:
            return check(root)
        return restore(root)
    except (OSError, PatchError) as error:
        print(f"CEFBROWSER_ZDF_STATIC_FIX=FAIL reason={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
