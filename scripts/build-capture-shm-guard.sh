#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler=${CAPTURE_CC:-clang}
linker=${CAPTURE_LD:-$(command -v ld.lld)}
output=${1:-app/LinuxRunner/GUISupport.bundle/capture-shm-guard.arm64}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/lr-capture-guard.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
"$compiler" --target=aarch64-linux-gnu --ld-path="$linker" -O2 \
    -ffreestanding -fno-builtin -fno-stack-protector -nostdinc -nostdlib -static \
    -Wall -Wextra -Werror -I. -Wl,-e,_start -Wl,--build-id=none -Wl,-z,noexecstack \
    app/LinuxRunner/GUISupport.bundle/capture-shm-guard.c -o "$temporary/guard"
python3 - "$temporary/guard" <<'PY'
import struct,sys
data=open(sys.argv[1],'rb').read()
assert data[:6]==b'\x7fELF\x02\x01'
assert struct.unpack_from('<HH',data,16)==(2,183),'static ARM64 executable required'
offset=struct.unpack_from('<Q',data,32)[0]
size,count=struct.unpack_from('<HH',data,54)
for i in range(count):
    kind,flags=struct.unpack_from('<II',data,offset+i*size)
    assert kind!=3,'PT_INTERP is forbidden'
    assert kind!=1 or flags&3!=3,'writable executable load segment'
    assert kind!=0x6474e551 or not flags&1,'executable stack'
PY
cp "$temporary/guard" "$output"
chmod 755 "$output"
echo 'Built freestanding ARM64 capture guard'
