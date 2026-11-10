//
//  iOSFS.m
//  iSH
//
//  Created by Noah Peeters on 26.10.19.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <sys/stat.h>
#include "SceneDelegate.h"
#include "iOSFS.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/path.h"
#include "fs/real.h"

int fakefs_bind_mount(const char *linux_path, const char *host_path, bool read_only);
int fakefs_bind_unmount(const char *linux_path);

const NSFileCoordinatorWritingOptions NSFileCoordinatorWritingForCreating = NSFileCoordinatorWritingForMerging;

@interface DirectoryPicker : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>

@property NSArray<NSURL *> *urls;
@property lock_t lock;
@property cond_t cond;

@end

@implementation DirectoryPicker

- (instancetype)init {
    if (self = [super init]) {
        lock_init(&_lock);
        cond_init(&_cond);
    }
    return self;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    [self documentPicker:controller didPickDocumentsAtURLs:@[]];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    [self documentPickerWasCancelled:(UIDocumentPickerViewController *)presentationController];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    lock(&_lock);
    self.urls = urls;
    notify(&_cond);
    unlock(&_lock);
}

- (int)askForURL:(NSURL **)url {
    TerminalViewController *terminalViewController = currentTerminalViewController;
    if (!terminalViewController)
        return _ENODEV;

    dispatch_async(dispatch_get_main_queue(), ^(void) {
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[ @"public.folder" ] inMode:UIDocumentPickerModeOpen];
        picker.delegate = self;
        if (@available(iOS 13, *)) {
        } else {
            picker.allowsMultipleSelection = YES;
        }
        picker.presentationController.delegate = self;
        [terminalViewController presentViewController:picker animated:true completion:nil];
    });

    lock(&_lock);
    while (_urls == nil) {
        int err = wait_for(&_cond, &_lock, NULL);
        if (err < 0) {
            unlock(&_lock);
            return err;
        }
    }
    NSArray<NSURL *> *urls = _urls;
    _urls = nil;
    unlock(&_lock);
    
    if (@available(iOS 13, *)) {
        assert(urls.count <= 1);
    }
    if (urls.count == 0)
        return _ECANCELED;
    *url = urls[0];
    return 0;
}

- (void)dealloc {
    cond_destroy(&_cond);
}

@end

static NSURL *url_for_mount(struct mount *mount) {
    return (__bridge NSURL *) mount->data;
}

static NSString *const kMountBookmarks = @"iOS Mount Bookmarks";
static NSString *const kExternalFolderMounts = @"iOS External Folder Mounts";
static NSString *const kExternalFolderMountBookmarkKey = @"bookmark";
static NSString *const kExternalFolderMountPathKey = @"path";
static NSString *const kExternalFolderMountDisplayNameKey = @"displayName";
#define BOOKMARK_PATH_ENCODING NSISOLatin1StringEncoding
// To avoid locking issues, only access from the main thread
static NSMutableDictionary<NSString *, NSData *> *ios_mount_bookmarks;
static NSMutableDictionary<NSString *, NSDictionary<NSString *, id> *> *external_folder_mounts;
static NSMutableDictionary<NSString *, NSURL *> *external_folder_mount_urls;
static bool mount_from_bookmarks = false; // This is a hack because I am bad at parameter passing
static void ensure_ios_mount_bookmarks_loaded(void) {
    if (ios_mount_bookmarks != nil)
        return;

    ios_mount_bookmarks = [NSUserDefaults.standardUserDefaults dictionaryForKey:kMountBookmarks].mutableCopy;
    if (ios_mount_bookmarks == nil)
        ios_mount_bookmarks = [NSMutableDictionary new];
}
static void sync_bookmarks(void) {
    ensure_ios_mount_bookmarks_loaded();
    [NSUserDefaults.standardUserDefaults setObject:ios_mount_bookmarks forKey:kMountBookmarks];
}
static void ensure_external_folder_mounts_loaded(void) {
    if (external_folder_mounts != nil)
        return;

    external_folder_mounts = [NSUserDefaults.standardUserDefaults dictionaryForKey:kExternalFolderMounts].mutableCopy;
    if (external_folder_mounts == nil)
        external_folder_mounts = [NSMutableDictionary new];
    external_folder_mount_urls = [NSMutableDictionary new];
}
static void sync_external_folder_mounts(void) {
    ensure_external_folder_mounts_loaded();
    [NSUserDefaults.standardUserDefaults setObject:external_folder_mounts forKey:kExternalFolderMounts];
}
static NSString *mount_point_for_external_folder_name(NSString *name) {
    return [@"/mnt" stringByAppendingPathComponent:name];
}
static NSError *external_folder_mount_error(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:@"io.carmo.ios-linuxkit.mounts"
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey: message}];
}
static NSString *sanitized_external_folder_name(NSString *name) {
    if (name.length == 0)
        name = @"folder";

    NSMutableString *result = [NSMutableString new];
    BOOL lastWasSeparator = NO;
    for (NSUInteger i = 0; i < name.length; i++) {
        unichar c = [name characterAtIndex:i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
            [result appendFormat:@"%C", c];
            lastWasSeparator = NO;
        } else if (c == '-' || !lastWasSeparator) {
            [result appendString:@"-"];
            lastWasSeparator = YES;
        }
    }

    NSCharacterSet *trimCharacters = [NSCharacterSet characterSetWithCharactersInString:@"-."];
    NSString *trimmed = [result stringByTrimmingCharactersInSet:trimCharacters];
    if (trimmed.length == 0 || [trimmed isEqualToString:@"."] || [trimmed isEqualToString:@".."])
        return @"folder";
    return trimmed;
}
static NSString *unique_external_folder_name(NSString *baseName) {
    ensure_external_folder_mounts_loaded();

    NSString *candidate = baseName;
    NSUInteger suffix = 2;
    while (external_folder_mounts[candidate] != nil) {
        candidate = [NSString stringWithFormat:@"%@-%lu", baseName, (unsigned long) suffix];
        suffix++;
    }
    return candidate;
}
static int bind_external_folder_mount(NSString *name, NSURL *url) {
    NSString *mountPoint = mount_point_for_external_folder_name(name);
    return fakefs_bind_mount(mountPoint.fileSystemRepresentation, url.path.fileSystemRepresentation, false);
}
static void apply_external_folder_mounts(void) {
    ensure_external_folder_mounts_loaded();

    BOOL updated = NO;
    for (NSString *name in external_folder_mounts.allKeys.copy) {
        NSDictionary<NSString *, id> *entry = external_folder_mounts[name];
        NSData *bookmark = entry[kExternalFolderMountBookmarkKey];
        if (![bookmark isKindOfClass:NSData.class]) {
            [external_folder_mounts removeObjectForKey:name];
            updated = YES;
            continue;
        }

        BOOL stale = NO;
        NSError *error = nil;
        NSURL *url = [NSURL URLByResolvingBookmarkData:bookmark
                                               options:0
                                         relativeToURL:nil
                                   bookmarkDataIsStale:&stale
                                                 error:&error];
        if (url == nil) {
            NSLog(@"restoring external folder mount %@ failed: %@", name, error);
            continue;
        }
        if (![url startAccessingSecurityScopedResource]) {
            NSLog(@"restoring external folder mount %@ failed: permission denied", name);
            continue;
        }

        int err = bind_external_folder_mount(name, url);
        if (err < 0) {
            NSLog(@"binding external folder mount %@ failed with error %d", name, err);
            [url stopAccessingSecurityScopedResource];
            continue;
        }

        external_folder_mount_urls[name] = url;
        if (stale) {
            NSData *updatedBookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
            if (updatedBookmark != nil) {
                NSMutableDictionary<NSString *, id> *updatedEntry = entry.mutableCopy;
                updatedEntry[kExternalFolderMountBookmarkKey] = updatedBookmark;
                updatedEntry[kExternalFolderMountPathKey] = url.path ?: @"";
                external_folder_mounts[name] = updatedEntry;
                updated = YES;
            }
        }
    }
    if (updated)
        sync_external_folder_mounts();
}
void iosfs_init(void) {
    ensure_ios_mount_bookmarks_loaded();

    mount_from_bookmarks = true;
    for (NSString *path in ios_mount_bookmarks.allKeys) {
        const char *point = [path cStringUsingEncoding:BOOKMARK_PATH_ENCODING];
        int err = do_mount(&iosfs, point, point, "", 0);
        if (err < 0) {
            NSLog(@"restoring bookmark %@ failed with error %d", path, err);
            [ios_mount_bookmarks removeObjectForKey:path];
        }
    }
    mount_from_bookmarks = false;
    sync_bookmarks();
    apply_external_folder_mounts();
}

void iosfs_clear_all_bookmarks(void) {
    ensure_ios_mount_bookmarks_loaded();
    [ios_mount_bookmarks removeAllObjects];
    sync_bookmarks();
    ensure_external_folder_mounts_loaded();
    for (NSString *name in external_folder_mounts.allKeys.copy) {
        fakefs_bind_unmount(mount_point_for_external_folder_name(name).fileSystemRepresentation);
        [external_folder_mount_urls[name] stopAccessingSecurityScopedResource];
    }
    [external_folder_mounts removeAllObjects];
    [external_folder_mount_urls removeAllObjects];
    sync_external_folder_mounts();
}

NSArray<NSDictionary<NSString *, NSString *> *> *iosfs_external_folder_mounts(void) {
    ensure_external_folder_mounts_loaded();

    NSMutableArray<NSDictionary<NSString *, NSString *> *> *mounts = [NSMutableArray new];
    NSArray<NSString *> *names = [external_folder_mounts.allKeys sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    for (NSString *name in names) {
        NSDictionary<NSString *, id> *entry = external_folder_mounts[name];
        NSString *displayName = [entry[kExternalFolderMountDisplayNameKey] isKindOfClass:NSString.class] ? entry[kExternalFolderMountDisplayNameKey] : name;
        NSString *path = [entry[kExternalFolderMountPathKey] isKindOfClass:NSString.class] ? entry[kExternalFolderMountPathKey] : @"";
        NSString *mountPoint = mount_point_for_external_folder_name(name);
        // Unavailable: access was not restored (revoked or bookmark failed) or the
        // folder is gone (e.g. an external drive was disconnected).
        BOOL isDirectory = NO;
        BOOL available = external_folder_mount_urls[name] != nil &&
            [NSFileManager.defaultManager fileExistsAtPath:external_folder_mount_urls[name].path isDirectory:&isDirectory] && isDirectory;
        [mounts addObject:@{
            @"name": name,
            @"displayName": displayName,
            @"path": path,
            @"mountPoint": mountPoint,
            @"available": available ? @"yes" : @"no",
        }];
    }
    return mounts;
}

// iOS side of a shared folder: the security-scoped URL in use, if access is held.
NSURL *iosfs_external_folder_url(NSString *name) {
    ensure_external_folder_mounts_loaded();
    return external_folder_mount_urls[name];
}

BOOL iosfs_add_external_folder_mount_url(NSURL *url, NSError **error) {
    ensure_external_folder_mounts_loaded();

    if (![url startAccessingSecurityScopedResource]) {
        if (error != NULL)
            *error = external_folder_mount_error(_EPERM, @"LinuxRunner does not have permission to access that folder.");
        return NO;
    }

    NSError *bookmarkError = nil;
    NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&bookmarkError];
    if (bookmark == nil) {
        [url stopAccessingSecurityScopedResource];
        if (error != NULL)
            *error = bookmarkError ?: external_folder_mount_error(_EINVAL, @"The folder could not be saved.");
        return NO;
    }

    NSString *baseName = sanitized_external_folder_name(url.lastPathComponent);
    NSString *name = unique_external_folder_name(baseName);
    int err = bind_external_folder_mount(name, url);
    if (err < 0) {
        [url stopAccessingSecurityScopedResource];
        if (error != NULL) {
            NSString *message = [NSString stringWithFormat:@"The folder could not be mounted at %@.", mount_point_for_external_folder_name(name)];
            *error = external_folder_mount_error(err, message);
        }
        return NO;
    }

    external_folder_mounts[name] = @{
        kExternalFolderMountBookmarkKey: bookmark,
        kExternalFolderMountPathKey: url.path ?: @"",
        kExternalFolderMountDisplayNameKey: url.lastPathComponent ?: name,
    };
    external_folder_mount_urls[name] = url;
    sync_external_folder_mounts();
    return YES;
}

void iosfs_remove_external_folder_mount(NSString *name) {
    ensure_external_folder_mounts_loaded();

    fakefs_bind_unmount(mount_point_for_external_folder_name(name).fileSystemRepresentation);
    [external_folder_mount_urls[name] stopAccessingSecurityScopedResource];
    [external_folder_mount_urls removeObjectForKey:name];
    [external_folder_mounts removeObjectForKey:name];
    sync_external_folder_mounts();
}

static int iosfs_mount(struct mount *mount) {
    NSURL *url = nil;
    if (mount_from_bookmarks) {
        NSString *bookmarkName = [NSString stringWithCString:mount->source encoding:BOOKMARK_PATH_ENCODING];
        url = [NSURL URLByResolvingBookmarkData:ios_mount_bookmarks[bookmarkName]
                                        options:0
                                  relativeToURL:nil
                            bookmarkDataIsStale:NULL
                                          error:nil];
        if (url != nil && ![url startAccessingSecurityScopedResource]) {
            return _EPERM;
        }
    }

    if (url == nil) {
        DirectoryPicker *picker = [DirectoryPicker new];
        int err = [picker askForURL:&url];
        if (err)
            return err;
        if (![url startAccessingSecurityScopedResource])
            return _EPERM;
    }

    // Overwrite url & base path
    mount->data = (void *) CFBridgingRetain(url);
    free((void *) mount->source);
    mount->source = strdup([[url path] UTF8String]);

    if (mount_param_flag(mount->info, "unsafe")) {
        mount->fs = &iosfs_unsafe;
    }

    if (!mount_from_bookmarks) {
        NSData *bookmark = [url bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
        NSString *path = [NSString stringWithCString:mount->point encoding:BOOKMARK_PATH_ENCODING];
        if (bookmark != nil) {
            dispatch_async(dispatch_get_main_queue(), ^{
                ios_mount_bookmarks[path] = bookmark;
                sync_bookmarks();
            });
        }
    }

    return realfs.mount(mount);
}

static int iosfs_umount(struct mount *mount) {
    NSString *path = [NSString stringWithCString:mount->point encoding:BOOKMARK_PATH_ENCODING];
    dispatch_async(dispatch_get_main_queue(), ^{
        [ios_mount_bookmarks removeObjectForKey:path];
        sync_bookmarks();
    });
    NSURL *url = url_for_mount(mount);
    [url stopAccessingSecurityScopedResource];
    CFBridgingRelease(mount->data);
    return 0;
}

static NSURL *url_for_path_in_mount(struct mount *mount, const char *path) {
    if (path[0] == '/')
        path++;
    return [url_for_mount(mount) URLByAppendingPathComponent:[NSString stringWithUTF8String:path] isDirectory:NO];
}

const char *path_for_url_in_mount(struct mount *mount, NSURL *url, const char *fallback) {
    NSString *mount_path = url_for_mount(mount).path;
    NSString *url_path = url.path;

    // The `/private` prefix is a special case as described in the documentation of `URLByStandardizingPath`.
    if ([mount_path hasPrefix:@"/private/"]) mount_path = [mount_path substringFromIndex:8];
    if ([url_path   hasPrefix:@"/private/"]) url_path   = [url_path   substringFromIndex:8];

    if (![url_path hasPrefix:mount_path])
        return fallback;

    return [url_path substringFromIndex:[mount_path length]].UTF8String;
}

static int iosfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat);
extern const struct fd_ops iosfs_fdops;

static int posixErrorFromNSError(NSError *error) {
    if (error == nil)
        return 0;
    while (error != nil) {
        if ([error.domain isEqualToString:NSPOSIXErrorDomain]) {
            return err_map((int) error.code);
        }
        error = error.userInfo[NSUnderlyingErrorKey];
    }
    return _EINVAL;
}

static int combine_error(NSError *coordinatorError, int err) {
    int posix_error = posixErrorFromNSError(coordinatorError);
    return posix_error ? posix_error : err;
}

static struct fd *iosfs_open(struct mount *mount, const char *path, int flags, int mode) {
    NSURL *url = url_for_path_in_mount(mount, path);

    // FIXME: this does a redundant file coordination operation
    struct statbuf stats;
    int err = iosfs_stat(mount, path, &stats);

    if (err == 0 && S_ISREG(stats.mode)) {
        NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];

        __block NSError *error = nil;
        __block struct fd *fd;
        __block dispatch_semaphore_t file_opened = dispatch_semaphore_create(0);

        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(void){
            void (^operation)(NSURL *url) = ^(NSURL *url) {
                fd = realfs_open(mount, path_for_url_in_mount(mount, url, path), flags, mode);

                if (IS_ERR(fd)) {
                    dispatch_semaphore_signal(file_opened);
                } else {
                    fd->ops = &iosfs_fdops;
                    dispatch_semaphore_t file_closed = dispatch_semaphore_create(0);
                    fd->data = (__bridge void *) file_closed;
                    dispatch_semaphore_signal(file_opened);
                    dispatch_semaphore_wait(file_closed, DISPATCH_TIME_FOREVER);
                }
            };

            int options;
            if (!(flags & O_WRONLY_) && !(flags & O_RDWR_)) {
                options = NSFileCoordinatorReadingWithoutChanges;
            } else if (flags & O_CREAT_) {
                options = NSFileCoordinatorWritingForCreating;
            } else {
                options = NSFileCoordinatorWritingForMerging;
            }
            [coordinator coordinateReadingItemAtURL:url options:options error:&error byAccessor:operation];
        });
        
        dispatch_semaphore_wait(file_opened, DISPATCH_TIME_FOREVER);

        int posix_error = posixErrorFromNSError(error);
        return posix_error ? ERR_PTR(posix_error) : fd;
    }
        
    struct fd *fd = realfs_open(mount, path, flags, mode);
    if (!IS_ERR(fd))
        fd->ops = &iosfs_fdops;
    return fd;
}

int iosfs_close(struct fd *fd) {
    int err = realfs.close(fd);
    if (fd->data != NULL) {
        dispatch_semaphore_t file_closed = (__bridge dispatch_semaphore_t) fd->data;
        dispatch_semaphore_signal(file_closed);
    }
    return err;
}

static int iosfs_rename(struct mount *mount, const char *src, const char *dst) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *src_url = url_for_path_in_mount(mount, src);
    NSURL *dst_url = url_for_path_in_mount(mount, dst);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:src_url options:NSFileCoordinatorWritingForMoving error:&error byAccessor:^(NSURL *url) {
        [coordinator itemAtURL:url willMoveToURL:dst_url];
        err = realfs.rename(mount, path_for_url_in_mount(mount, url, src), dst);
        [coordinator itemAtURL:url didMoveToURL:dst_url];
    }];

    return combine_error(error, err);
}

static int iosfs_symlink(struct mount *mount, const char *target, const char *link) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *dst_url = url_for_path_in_mount(mount, link);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:dst_url options:NSFileCoordinatorWritingForCreating error:&error byAccessor:^(NSURL *url) {
        err = realfs.symlink(mount, path_for_url_in_mount(mount, url, target), link);
    }];

    return combine_error(error, err);
}

static int iosfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingForCreating error:&error byAccessor:^(NSURL *url) {
        err = realfs.mknod(mount, path_for_url_in_mount(mount, url, path), mode, dev);
    }];

    return combine_error(error, err);
}

static int iosfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingContentIndependentMetadataOnly error:&error byAccessor:^(NSURL *url) {
        err = realfs.setattr(mount, path_for_url_in_mount(mount, url, path), attr);
    }];

    return combine_error(error, err);
}

static int iosfs_fsetattr(struct fd *fd, struct attr attr) {
    return realfs.fsetattr(fd, attr);
}

static ssize_t iosfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block ssize_t size;

    [coordinator coordinateReadingItemAtURL:in_url options:NSFileCoordinatorReadingWithoutChanges error:&error byAccessor:^(NSURL *url) {
        size = realfs.readlink(mount, path_for_url_in_mount(mount, url, path), buf, bufsize);
    }];

    int posix_error = posixErrorFromNSError(error);
    return posix_error ? posix_error : size;
}

static int iosfs_getpath(struct fd *fd, char *buf) {
    return realfs.getpath(fd, buf);
}

static int iosfs_link(struct mount *mount, const char *src, const char *dst) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *dst_url = url_for_path_in_mount(mount, dst);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:dst_url options:NSFileCoordinatorWritingForCreating error:&error byAccessor:^(NSURL *url) {
        err = realfs.link(mount, src, path_for_url_in_mount(mount, url, dst));
    }];

    return combine_error(error, err);
}

static int iosfs_unlink(struct mount *mount, const char *path) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingForDeleting error:&error byAccessor:^(NSURL *url) {
        err = realfs.unlink(mount, path_for_url_in_mount(mount, url, path));
    }];

    return combine_error(error, err);
}

static int iosfs_rmdir(struct mount *mount, const char *path) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingForDeleting error:&error byAccessor:^(NSURL *url) {
        err = realfs.rmdir(mount, path_for_url_in_mount(mount, url, path));
    }];

    return combine_error(error, err);
}

static int iosfs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateReadingItemAtURL:in_url options:NSFileCoordinatorReadingWithoutChanges error:&error byAccessor:^(NSURL *url) {
        err = realfs.stat(mount, path_for_url_in_mount(mount, url, path), fake_stat);
    }];

    return combine_error(error, err);
}

static int iosfs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    int err = realfs.fstat(fd, fake_stat);
    return err;
}

static int iosfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingContentIndependentMetadataOnly error:&error byAccessor:^(NSURL *url) {
        err = realfs.utime(mount, path_for_url_in_mount(mount, url, path), atime, mtime, follow_links);
    }];

    return combine_error(error, err);
}

static int iosfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSURL *in_url = url_for_path_in_mount(mount, path);

    NSError *error;
    __block int err;

    [coordinator coordinateWritingItemAtURL:in_url options:NSFileCoordinatorWritingForCreating error:&error byAccessor:^(NSURL *url) {
        err = realfs.mkdir(mount, path_for_url_in_mount(mount, url, path), mode);
    }];

    return combine_error(error, err);
}

static int iosfs_flock(struct fd *fd, int operation) {
    return realfs.flock(fd, operation);
}

const struct fs_ops iosfs = {
    .name = "ios", .magic = 0x694f5320,
    .mount = iosfs_mount,
    .umount = iosfs_umount,
    .statfs = realfs_statfs,

    .open = iosfs_open,
    .readlink = iosfs_readlink,
    .link = iosfs_link,
    .unlink = iosfs_unlink,
    .rmdir = iosfs_rmdir,
    .rename = iosfs_rename,
    .symlink = iosfs_symlink,
    .mknod = iosfs_mknod,

    .close = iosfs_close,
    .stat = iosfs_stat,
    .fstat = iosfs_fstat,
    .setattr = iosfs_setattr,
    .fsetattr = iosfs_fsetattr,
    .utime = iosfs_utime,
    .getpath = iosfs_getpath,
    .flock = iosfs_flock,

    .mkdir = iosfs_mkdir,
};

const struct fs_ops iosfs_unsafe = {
    .name = "ios-unsafe", .magic = 0x694f5321,
    .mount = iosfs_mount,
    .umount = iosfs_umount,
    .statfs = realfs_statfs,

    .open = realfs_open,
    .readlink = realfs_readlink,
    .link = realfs_link,
    .unlink = realfs_unlink,
    .rmdir = realfs_rmdir,
    .rename = realfs_rename,
    .symlink = realfs_symlink,
    .mknod = realfs_mknod,

    .close = realfs_close,
    .stat = realfs_stat,
    .fstat = realfs_fstat,
    .setattr = realfs_setattr,
    .fsetattr = realfs_fsetattr,
    .utime = realfs_utime,
    .getpath = realfs_getpath,
    .flock = realfs_flock,

    .mkdir = realfs_mkdir,
};

const struct fd_ops iosfs_fdops = {
    .read = realfs_read,
    .write = realfs_write,
    .readdir = realfs_readdir,
    .telldir = realfs_telldir,
    .seekdir = realfs_seekdir,
    .lseek = realfs_lseek,
    .mmap = realfs_mmap,
    .poll = realfs_poll,
    .fsync = realfs_fsync,
    .close = iosfs_close,
    .getflags = realfs_getflags,
    .setflags = realfs_setflags,
};
