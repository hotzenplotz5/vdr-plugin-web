# ZDF HbbTV controls/input static override

## Live ARD failure found on 2026-09-29

The live session had already navigated to `http://localhost/internal/blank/page`
while the external ARD video continued. PAUSE/STOP produced
`window.cefKeyPress is not a function`. The provider log showed local OSD/player
destruction at video start. cefbrowser's `checkVdrRegular()` navigates to the
blank page whenever the provider's `IsWebActive()` reports false.

The provider now keeps the browser alive for owned starting/active/closing Suite
sessions, captures raw/QOI browser frames directly into the presentation store,
and bypasses local player/OSD replacement for Suite video starts. Legacy local
playback retains its existing path. This part requires a rebuilt VDR plugin;
the static installer alone cannot repair an already blank browser session.
No service restart is performed by the installer. Real-device acceptance of
controls and media effects remains required after loading the rebuilt plugin.

The real CEF process also rewrites `/proc/PID/cmdline` into one space-separated
process title. Detection handles that form as well as normal NUL-separated argv;
the real PID 1946 resolves to `/var/lib/hbbtv/cefbrowser`.

This directory documents the VDR-Suite runtime correction for the cefbrowser
static-content boundary used by `vdr-plugin-web`.

## Pinned upstream

The correction is written against:

`Zabrimus/cefbrowser@d14517be4a7aecfc3ba0b77831af237721ef48e9`

No cefbrowser executable patch is required. cefbrowser serves and injects
`static-content/js/*.js` and `static-content/css/*.css` from its configured
`--staticPath`, `--staticpath` or `-s` (or, when omitted, from the executable directory).

## Root cause covered by this override

For `*-hbbtv.zdf.de`, upstream `video_quirks.js` hides the complete
`#root` tree when external video starts. That also hides native ZDF controls.
At the same time, upstream `cefKeyPress()` dispatches synthetic HbbTV media
and colour keys on `document`, so element/root handlers do not receive those
events as descendants.

The override keeps the ZDF application tree alive:

- only actual browser `<video>` pixels receive the dedicated hidden class;
- the video's ancestor chain up to `#root` is made background-transparent,
  allowing the external VDR-Suite media plane to remain visible;
- native application controls remain visible and focusable;
- synthetic ZDF HbbTV keys dispatch to the active element when possible, or to
  `#root`, and then bubble to document/window listeners;
- non-ZDF key dispatch remains unchanged.

The native CEF key path for ENTER/arrows is intentionally not rewritten in this
slice. Keeping the ZDF root visible preserves its focus tree; real acceptance
must prove ENTER/arrows before any additional CEF focus change is justified.

## Apply / verify / rollback

`tools/install_cefbrowser_zdf_controls_fix.sh` auto-detects the running
cefbrowser static root from `--staticPath`, `--staticpath` or `-s`, otherwise from
the executable directory. Both separate values and attached values are supported.
It excludes CEF children with `--type=...` (also `--type VALUE`), prefers a unique
main process with PPID 1, then a unique root of the remaining browser processes.
Multiple equally preferred main processes, unreadable browser metadata, and
empty or conflicting static options stop installation before any file changes.
No manual PID selection is required. Relative paths resolve against the selected
process's working directory. The real yaVDR main process (PID 1946, PPID 1) with
`--staticpath=/var/lib/hbbtv/cefbrowser` therefore selects exactly that root,
ignoring its zygote/GPU/utility/renderer children.

The Python patcher validates exact upstream source markers before it
writes anything, stages replacements, preserves one backup beside each source
file and verifies all postconditions.

Apply:

```sh
tools/install_cefbrowser_zdf_controls_fix.sh
```

Verify an explicit root:

```sh
python3 tools/apply_cefbrowser_zdf_controls_fix.py --root /path/to/static-content --check
```

Rollback:

```sh
python3 tools/apply_cefbrowser_zdf_controls_fix.py --root /path/to/static-content --restore
```

A running HbbTV page has already loaded the old scripts. After applying the
override, close HbbTV and start a fresh application session. No VDR,
vdr-suite-daemon or cefbrowser restart is part of this installer.
