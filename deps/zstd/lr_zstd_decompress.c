/* Decompression-only build of upstream zstd 1.5.7 (BSD, see LICENSE), used by
 * the SteamOS image reader. Mirrors build/single_file_libs/zstddeclib-in.c. */
#define DEBUGLEVEL 0
#define MEM_MODULE
#undef  XXH_NAMESPACE
#define XXH_NAMESPACE ZSTD_
#undef  XXH_PRIVATE_API
#define XXH_PRIVATE_API
#undef  XXH_INLINE_ALL
#define XXH_INLINE_ALL
#define ZSTD_LEGACY_SUPPORT 0
#define ZSTD_STRIP_ERROR_STRINGS
#define ZSTD_TRACE 0
#define ZSTD_DISABLE_ASM 1
#define ZSTD_DEPS_NEED_MALLOC
#include "lib/common/zstd_deps.h"
#include "lib/common/debug.c"
#include "lib/common/entropy_common.c"
#include "lib/common/error_private.c"
#include "lib/common/fse_decompress.c"
#include "lib/common/zstd_common.c"
#include "lib/decompress/huf_decompress.c"
#include "lib/decompress/zstd_ddict.c"
#include "lib/decompress/zstd_decompress.c"
#include "lib/decompress/zstd_decompress_block.c"
