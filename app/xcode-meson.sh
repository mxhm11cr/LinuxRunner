#!/bin/bash

jit=false
jit_emit=false
# JIT-capable app variant (LINUXRUNNER_JIT=YES): the translator is compiled in
# but only runs once iOS grants executable memory; the default build stays
# interpreter-only because the JIT build's gadgets are slower when it is off.
if [[ $# -eq 0 && ${LINUXRUNNER_JIT:-NO} == YES ]]; then
    jit=true
    jit_emit=true
fi
if [[ $# -ne 0 ]]; then
    if [[ $# -ne 1 || $1 != --aot-bootstrap || ${TARGET_NAME:-} != iSH-ARM64-AOT-Bootstrap || ${ARCHS:-} != arm64 || ${MESON_BUILD_DIR:-} != */meson-aot-bootstrap ]]; then
        echo 'AOT bootstrap requires its isolated ARM64 target and Meson directory' >&2
        exit 2
    fi
    for define in GUEST_ARM64=1 ISH_JIT=1 ISH_JIT_NO_EMIT=1 ISH_AOT_BOOTSTRAP=1; do
        case " ${GCC_PREPROCESSOR_DEFINITIONS:-} " in
            *" $define "*) ;;
            *) echo "AOT bootstrap is missing shared definition: $define" >&2; exit 2 ;;
        esac
    done
    jit=true
fi

# Try to figure out the user's PATH to pick up their installed utilities.
# Do not use sudo here: personal Apple developer machines are not always
# configured with sudo for the GUI user, and the build only needs PATH hints.
login_path=$(env -i HOME="$HOME" USER="$USER" SHELL="${SHELL:-/bin/zsh}" /bin/zsh -lc 'print -r -- $PATH' 2>/dev/null || true)
if [[ -n "$login_path" ]]; then
    export PATH="$PATH:$login_path"
fi

# Optional toolchain bundle; normal builds use tools already on PATH.
if [ -n "${LINUXRUNNER_TOOLCHAIN_ROOT:-}" ]; then
    terax_tools=$LINUXRUNNER_TOOLCHAIN_ROOT
    terax_lld=$terax_tools/.tmp-lld-bottle/lld/22.1.5
    terax_llvm=$terax_tools/.tmp-llvm-bottle/llvm/22.1.5
    terax_z3=$terax_tools/.tmp-homebrew/Cellar/z3/4.15.4
    terax_zstd=$terax_tools/.tmp-homebrew/Cellar/zstd/1.5.7_1
    terax_lz4=$terax_tools/.tmp-homebrew/Cellar/lz4/1.10.0
    if [ -x "$terax_lld/bin/ld.lld" ] && [ -d "$terax_llvm/lib" ]; then
        export PATH="$terax_lld/bin:$PATH"
        export DYLD_LIBRARY_PATH="$terax_lld/lib:$terax_llvm/lib:$terax_z3/lib:$terax_zstd/lib:$terax_lz4/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    fi
fi

mkdir -p "$MESON_BUILD_DIR"
cd "$MESON_BUILD_DIR"

config=$(meson introspect --buildoptions)
if [[ $? -ne 0 ]]; then
    export CC_FOR_BUILD="env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET xcrun clang"
    export CC="$CC_FOR_BUILD" # compatibility with meson < 0.54.0
    crossfile=cross.txt
    for arch in $ARCHS; do
        arch_args="'-arch', '$arch', $arch_args"
    done
    arch_args="${arch_args%%, }"
    meson_arch=${ARCHS%% *}
    case "$meson_arch" in
        arm64) meson_arch=aarch64 ;;
    esac
    cat | tee $crossfile <<-EOF
    [binaries]
    c = 'clang'
    ar = 'ar'

    [host_machine]
    system = 'darwin'
    cpu_family = '$meson_arch'
    cpu = '$meson_arch'
    endian = 'little'

    [built-in options]
    c_args = [$arch_args]
    
    [properties]
    needs_exe_wrapper = true
EOF
    guest_arch_opt=""
    if [[ -n "$GUEST_ARCH" ]]; then
        guest_arch_opt="-Dguest_arch=$GUEST_ARCH"
    fi
    (set -x; meson "$SRCROOT" --cross-file "$crossfile" $guest_arch_opt \
        -Djit=$jit -Djit_emit=$jit_emit -Dcli_aot=) || exit $?
    config=$(meson introspect --buildoptions)
fi

buildtype=debug
b_ndebug=false
if [[ $CONFIGURATION == Release ]]; then
    buildtype=debugoptimized
fi
b_sanitize=none
if [[ -n "$ENABLE_ADDRESS_SANITIZER" ]]; then
    b_sanitize=address
fi
log=$ISH_LOG
log_handler=$ISH_LOGGER
kernel=ish
if [[ -n "$ISH_KERNEL" ]]; then
    kernel=$ISH_KERNEL
fi
kconfig=""
guest_arch=${GUEST_ARCH:-arm64}
# Reset backend, emission and CLI images together, including in reused builds.
# Only the isolated bootstrap target may compile the native backend; it still
# installs the native fault adapter; static images are linked separately only
# when the checked Apple image build is explicitly requested.
(set -x; meson configure -Djit=$jit -Djit_emit=$jit_emit -Dcli_aot= -Dvdso_required=true) || exit $?
for var in buildtype log b_ndebug b_sanitize log_handler kernel kconfig guest_arch; do
    old_value=$(python3 -c "import sys, json; v = next(x['value'] for x in json.load(sys.stdin) if x['name'] == '$var'); print(str(v).lower() if isinstance(v, bool) else ','.join(v) if isinstance(v, list) else v)" <<< $config)
    new_value=${!var}
    if [[ $old_value != $new_value ]]; then
        set -x; meson configure "-D$var=$new_value" || exit $?
    fi
done
