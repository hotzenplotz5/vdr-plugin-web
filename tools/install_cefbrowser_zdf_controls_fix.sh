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
    PIDS="$(pgrep -x cefbrowser 2>/dev/null)"
    COUNT="$(printf '%s\n' "$PIDS" | sed '/^$/d' | wc -l | tr -d ' ')"

    if test "$COUNT" = "0"; then
        echo "STOP: kein laufender cefbrowser gefunden; static root als Argument angeben"
        CONTINUE=0
    elif test "$COUNT" != "1"; then
        echo "STOP: mehrere cefbrowser-Prozesse gefunden; static root als Argument angeben"
        printf '%s\n' "$PIDS"
        CONTINUE=0
    else
        PID="$PIDS"
        NEXT_IS_STATIC=0

        while IFS= read -r ARG; do
            if test "$NEXT_IS_STATIC" = "1"; then
                STATIC_ROOT="$ARG"
                NEXT_IS_STATIC=0
                continue
            fi

            case "$ARG" in
                --staticPath=*)
                    STATIC_ROOT="${ARG#--staticPath=}"
                    ;;
                --staticPath|-s)
                    NEXT_IS_STATIC=1
                    ;;
                -s*)
                    STATIC_ROOT="${ARG#-s}"
                    ;;
            esac
        done < <(tr '\0' '\n' < "/proc/$PID/cmdline")

        if test -z "$STATIC_ROOT"; then
            EXE="$(readlink -f "/proc/$PID/exe" 2>/dev/null)"
            if test -z "$EXE"; then
                echo "STOP: cefbrowser executable konnte nicht aufgelöst werden"
                CONTINUE=0
            else
                STATIC_ROOT="$(dirname "$EXE")"
            fi
        fi
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
