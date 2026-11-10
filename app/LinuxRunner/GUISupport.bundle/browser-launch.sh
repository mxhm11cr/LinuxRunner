#!/bin/sh
set -eu
cd "$(dirname "$0")"
/bin/sh ./browser-preflight.sh "${1:-}"
# Browsers keep their validated software rendering. A desktop-wide GPU mode
# leaves the Mesa bridge preloaded; switch it to pass-through for them.
export LR_MESA_METAL_MODE=off
case "${1:-}" in
 chromium)
    export QT_QPA_PLATFORM=xcb QT_OPENGL=software LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe
    export QT_QUICK_BACKEND=software QSG_RHI_BACKEND=null
    # Supported diagnostic mode: guest Linux sandbox unavailable, iOS intact.
    export QTWEBENGINE_DISABLE_SANDBOX=1
    export QTWEBENGINE_CHROMIUM_FLAGS='--no-sandbox --disable-gpu --js-flags=--jitless --enable-logging=stderr --log-level=0'
    echo 'BROWSER: development Linux sandbox disabled; interpreter backend; V8 jitless; software display'
    shift; exec ./browser "$@"
    ;;
 firefox)
    launcher=$(command -v firefox || command -v firefox-esr)
    mkdir -p "$HOME/.local/share/linuxrunner-firefox"
    shift
    [ "$#" -gt 0 ] || set -- "file://$PWD/firefox-test.html"
    exec /bin/sh ./firefox-launch.sh "$launcher" --no-remote --profile "$HOME/.local/share/linuxrunner-firefox" "$@"
    ;;
 *) echo 'Expected chromium or firefox'; exit 2;;
esac
