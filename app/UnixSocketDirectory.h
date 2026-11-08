#import <Foundation/Foundation.h>
#include <sys/stat.h>

// Darwin's /var alias can save eight bytes in the physical-device sandbox
// temp path. Use it only after verifying that it names the same directory.
static inline NSString *LRUnixSocketDirectory(NSString *directory) {
    if (![directory hasPrefix:@"/private/var/"]) return directory;
    NSString *shorter = [directory substringFromIndex:8];
    struct stat original, alias;
    if (stat(directory.fileSystemRepresentation, &original) == 0 &&
        stat(shorter.fileSystemRepresentation, &alias) == 0 &&
        S_ISDIR(original.st_mode) && S_ISDIR(alias.st_mode) &&
        original.st_dev == alias.st_dev && original.st_ino == alias.st_ino)
        return shorter;
    return directory;
}
