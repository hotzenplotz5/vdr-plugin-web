#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)"
PATCHER="$SCRIPT_DIR/apply_cefbrowser_zdf_controls_fix.py"
STATIC_ROOT="$1"
CONTINUE=1

echo "=== VDR-Suite / cefbrowser ZDF Controls+Input Static Fix ==="

if test ! -x "$PATCHER"; then
    echo "STOP: patcher fehlt oder ist nicht ausführbar: $PATCHER"
    CONTINUE=0
fi

if test "$CONTINUE" = "1" && test -z "$STATIC_ROOT"; then
    STATIC_ROOT="$(python3 "$SCRIPT_DIR/detect_cefbrowser_static_root.py")"
    if test "$?" != "0"; then
        CONTINUE=0
    fi
fi

if test "$CONTINUE" = "1"; then
    echo "STATIC_ROOT=$STATIC_ROOT"

    if test ! -f "$STATIC_ROOT/js/video_quirks.js"         || test ! -f "$STATIC_ROOT/js/keyhandler.js"         || test ! -f "$STATIC_ROOT/css/videoquirks.css"; then
        echo "STOP: static root enthält nicht die erwarteten cefbrowser-Dateien"
        CONTINUE=0
    fi
fi

if test "$CONTINUE" = "1"; then
    python3 "$PATCHER" --root "$STATIC_ROOT" --apply
    RC=$?

    if test "$RC" != "0"; then
        echo "STOP: Static-Fix konnte nicht sicher angewendet werden"
        CONTINUE=0
    fi
fi

if test "$CONTINUE" = "1"; then
    python3 "$PATCHER" --root "$STATIC_ROOT" --check
    RC=$?

    if test "$RC" != "0"; then
        echo "STOP: Verifikation des Static-Fix fehlgeschlagen"
        CONTINUE=0
    fi
fi

if test "$CONTINUE" = "1"; then
    echo
    echo "============================================"
    echo "CEFBROWSER_ZDF_CONTROLS_INPUT_INSTALL=PASS"
    echo "STATIC_ROOT=$STATIC_ROOT"
    echo "============================================"
    echo
    echo "Kein VDR-, Daemon- oder cefbrowser-Neustart wurde ausgeführt."
    echo "Eine bereits geladene HbbTV-Seite benutzt noch die alten Skripte."
    echo "HbbTV daher sauber beenden und anschließend frisch starten."
fi

if test "$CONTINUE" != "1"; then
    exit 1
fi
