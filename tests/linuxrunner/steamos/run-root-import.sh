#!/bin/bash
# Host-only disposable importer fixture. Requires CMake and pinned libarchive.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=build/root-import-test
mkdir -p "$out/RootImportTests.app/Contents/MacOS"
cmake -S deps/libarchive -B "$out/archive" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_TEST=OFF -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
    -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_LZMA=OFF -DENABLE_BZip2=OFF \
    -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF
cmake --build "$out/archive" --target archive_static -j 4
for source in tools/fakefs.c fs/fake-db.c fs/fake-migrate.c fs/fake-rebuild.c \
              util/fchdir.c app/LinuxRunner/FileTransfer.c; do
    xcrun clang -O1 -g -DGUEST_ARM64=1 -I. -Ideps/libarchive/libarchive \
        -c "$source" -o "$out/$(basename "$source" .c).o"
done
cat > "$out/host-log.c" <<'C'
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
void ish_printk(const char *f, ...) { va_list a; va_start(a,f); vfprintf(stderr,f,a); va_end(a); }
_Noreturn void die(const char *f, ...) { va_list a; va_start(a,f); vfprintf(stderr,f,a); va_end(a); abort(); }
C
xcrun clang -c "$out/host-log.c" -o "$out/host-log.o"
xcrun clang -O1 -g -fobjc-arc -DLINUXRUNNER=1 -DGUEST_ARM64=1 -I. \
    tests/linuxrunner/steamos/RootImportTests.m app/Roots.m app/NSObject+SaneKVO.m \
    "$out"/*.o "$out/archive/libarchive/libarchive.a" \
    -framework Foundation -lsqlite3 -lz -liconv \
    -o "$out/RootImportTests.app/Contents/MacOS/RootImportTests"
exec "$out/RootImportTests.app/Contents/MacOS/RootImportTests"
