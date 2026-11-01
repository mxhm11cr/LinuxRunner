#!/bin/sh
# Run inside a disposable ARM64 guest with a compiler; do not modify guest libs.
set -eu
cd "$(dirname "$0")"
gcc -O2 -Wall -Wextra -pthread -Wl,-z,noexecstack stack.c -o stack-nx
gcc -O2 -Wall -Wextra -pthread -Wl,-z,execstack stack.c -o stack-x
gcc -O2 -Wall -Wextra -fPIC -shared -Wl,-z,execstack library.c -o library.so
gcc -O2 -Wall -Wextra -pthread -DSTARTUP_LIBRARY -Wl,-z,noexecstack stack.c -L. -Wl,-rpath,'$ORIGIN' -l:library.so -o startup-library
gcc -O2 -Wall -Wextra -Wl,-z,noexecstack load.c -ldl -o load-nx
gcc -O2 -Wall -Wextra -Wl,-z,execstack load.c -ldl -o load-x
for n in stack-nx stack-x library.so startup-library; do readelf -lW "$n" | sed -n /GNU_STACK/p; done
./stack-nx 0
# musl does not implement glibc's dynamic executable-stack upgrade policy.
if getconf GNU_LIBC_VERSION 2>/dev/null | grep -q glibc; then
 ./stack-x 1
 ./startup-library 1
 ./load-x ./library.so allow
 version=$(getconf GNU_LIBC_VERSION | awk '{print $2}')
 major=${version%%.*}; minor=${version#*.}; minor=${minor%%.*}
 if [ "$major" -gt 2 ] || { [ "$major" -eq 2 ] && [ "$minor" -ge 41 ]; }; then
  ./load-nx ./library.so reject
 else
  ./load-nx ./library.so allow
 fi
else
 # musl deliberately creates non-executable pthread stacks; PT_GNU_STACK
 # controls the initial kernel stack, not arbitrary later anonymous mappings.
 LR_THREAD_EXPECT_EXEC=0 ./stack-x 1
 echo 'DYNAMIC_EXECSTACK_POLICY_NOT_TESTED libc=musl (not a glibc policy pass)'
fi
printf 'ELF_STACK_SUITE_PASS\n'
