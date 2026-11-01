#!/bin/sh
# JIT smoke test: real dynamically linked programs run with native translation
# in both executable-memory modes, and /proc/ish/jit must report "mode: JIT"
# (an interpreter fallback is a failure, not a pass).
# Usage: jit_smoke.sh ISH_JIT_RUNTIME ROOTFS [ROOTFS...]
# Regression for the JIT's LDUR offset wrap (fiber_block fields lie more than
# 256 bytes before code[]): every linked block exit wrote code-stream words
# into the guest PC and the dynamic loader crashed.
set -u
runtime=$1; shift
status=0
for rootfs in "$@"; do
    for mode in mapjit dualmap; do
        if [ "$mode" = dualmap ]; then dual=1; else dual=; fi
        out=$(env ISH_JIT=1 ${dual:+ISH_JIT_DUALMAP=1} "$runtime" -f "$rootfs" /bin/sh -c '
            echo "sum=$(seq 1 2000 | awk "{s+=\$1} END {print s}")"
            printf "b\na\nc\n" | sort | head -1
            head -1 /proc/ish/jit' 2>&1)
        ok=1
        printf '%s\n' "$out" | grep -qx 'sum=2001000' || ok=0
        printf '%s\n' "$out" | grep -qx 'a' || ok=0
        printf '%s\n' "$out" | grep -qx 'mode: JIT' || ok=0
        printf '%s\n' "$out" | grep -q 'GUEST_FAULT' && ok=0
        if [ $ok = 1 ]; then echo "JIT_SMOKE PASS $mode $rootfs"; else echo "JIT_SMOKE FAIL $mode $rootfs"; printf '%s\n' "$out" | grep -E 'GUEST_FAULT|mode:' | head -3; status=1; fi
    done
done
exit $status
