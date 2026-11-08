//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Roots.h"
#import "AppGroup.h"
#import "NSObject+SaneKVO.h"
#include "tools/fakefs.h"
#ifdef LINUXRUNNER
#import <CommonCrypto/CommonDigest.h>
#import "LinuxRunner/FileTransfer.h"
#import "LinuxRunner/BootLog.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
static BOOL validRunnerRoot(NSString *name) {
    return lr_valid_leaf(name.UTF8String) &&
        strlen(name.UTF8String) == [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}
#endif

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        [manager createDirectoryAtURL:rootsDir
          withIntermediateDirectories:YES
                           attributes:@{}
                                error:nil];
    });
    return rootsDir;
}

static NSString *kDefaultRoot = @"Default Root";

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL wantsVersionFile;
#ifdef LINUXRUNNER
@property NSError *initializationError;
#endif
@end

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        NSError *error = nil;
        NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:RootsDir().path error:&error];
#ifdef LINUXRUNNER
        if (error) { self.initializationError = error; self.roots = [NSMutableOrderedSet new]; LRBootEvent("root directory unavailable"); return self; }
#else
        NSAssert(error == nil, @"couldn't list roots: %@", error);
#endif
        self.roots = [[NSMutableOrderedSet alloc] initWithArray:rootNames ?: @[]];
        if (!self.roots.count) {
            // import default root
            NSError *error;
            if (![self importRootFromArchive:[NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"]
                                        name:@"default"
                                       error:&error
                            progressReporter:nil]) {
#ifdef LINUXRUNNER
                self.initializationError = error ?: [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
                LRBootEvent("rootfs import failed"); return self;
#else
                NSAssert(NO, @"failed to import default root, error %@", error);
#endif
            }
            _wantsVersionFile = YES;
        }
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self syncFileProviderDomains];
        }];
        [self syncFileProviderDomains];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = self.roots.firstObject;
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (void)syncFileProviderDomains {
#ifdef LINUXRUNNER
    // No extension is embedded in this target; avoid unrelated provider APIs.
    return;
#else
    if (self.updatingDomains) {
        self.domainsNeedUpdate = YES;
        return;
    }
    self.updatingDomains = YES;
    self.domainsNeedUpdate = NO;

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSMutableOrderedSet<NSString *> *missingRoots = [self.roots mutableCopy];
        for (NSFileProviderDomain *domain in domains) {
            if ([missingRoots containsObject:domain.identifier]) {
                [missingRoots removeObject:domain.identifier];
            } else {
                [NSFileManager.defaultManager removeItemAtURL:
                 [NSFileProviderManager.defaultManager.documentStorageURL
                  URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                        error:nil];
                [NSFileProviderManager removeDomain:domain completionHandler:onError];
            }
        }
        for (NSString *rootId in missingRoots) {
            [NSFileProviderManager addDomain:[[NSFileProviderDomain alloc] initWithIdentifier:rootId
                                                                                  displayName:rootId
                                                                pathRelativeToDocumentStorage:rootId]
                           completionHandler:onError];
        }
        if (self.domainsNeedUpdate)
            [self syncFileProviderDomains];
        self.updatingDomains = NO;
    }];
#endif
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

#ifdef LINUXRUNNER
static void root_import_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    root_progress_callback(cookie, .2 + .8 * progress, message, should_cancel);
}
#endif

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
#ifdef LINUXRUNNER
    return [self importVerifiedRootFromArchive:archive name:name sha256:[NSBundle.mainBundle objectForInfoDictionaryKey:@"LinuxRunnerRootfsSHA256"] metadata:@{@"distributionID":@"alpine",@"distribution":@"Alpine Linux",@"version":@"3.24.2",@"packageManager":@"apk",@"defaultShell":@"/bin/sh"} error:error progressReporter:progress];
}
- (BOOL)importVerifiedRootFromArchive:(NSURL *)archive name:(NSString *)name sha256:(NSString *)expected metadata:(NSDictionary *)metadata error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    return [self importVerifiedRootFromArchive:archive name:name sha256:expected topLevel:nil metadata:metadata error:error progressReporter:progress];
}
// topLevel: the archive's single top-level directory, imported as the root;
// an entry outside it fails the import.
- (BOOL)importVerifiedRootFromArchive:(NSURL *)archive name:(NSString *)name sha256:(NSString *)expected topLevel:(NSString * _Nullable)topLevel metadata:(NSDictionary *)metadata error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
#endif
#ifndef LINUXRUNNER
    NSString *topLevel = nil;
#endif
#ifdef LINUXRUNNER
    if (!validRunnerRoot(name) || [self.roots containsObject:name]) {
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:@"Invalid or duplicate environment root identifier."}];
        return NO;
    }
    // Protect every importer, including the retained upstream filesystem UI.
    // The normal importer admits the pinned Alpine image. The managed Debian
    // installer supplies its archive hash only after authenticating every package.
    // Verify a private snapshot, then import those exact bytes. A document
    // provider may change its original URL while verification is in progress.
    unsigned long long maximumBytes = 512ULL * 1024 * 1024;
    unsigned long long pinnedBytes = 0;
    if ([metadata[@"distributionID"] isEqual:@"steamos"]) {
        NSURL *support = [NSBundle.mainBundle URLForResource:@"SteamOSSupport" withExtension:@"bundle"];
        NSData *manifestData = support ? [NSData dataWithContentsOfURL:[support URLByAppendingPathComponent:@"manifest.json"]] : nil;
        id decoded = manifestData ? [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil] : nil;
        NSDictionary *manifest = [decoded isKindOfClass:NSDictionary.class] ? decoded : nil;
        pinnedBytes = [manifest[@"archiveBytes"] unsignedLongLongValue];
        if (![manifest isKindOfClass:NSDictionary.class] || [manifest[@"schema"] integerValue] != 1 ||
            ![manifest[@"architecture"] isEqual:@"arm64"] || ![manifest[@"archiveSHA256"] isEqual:expected] ||
            ![manifest[@"sourceImageSHA256"] isEqual:metadata[@"sourceImageSHA256"]] ||
            !pinnedBytes || pinnedBytes > 6ULL * 1024 * 1024 * 1024) {
            if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
                userInfo:@{NSLocalizedDescriptionKey:@"The SteamOS archive must match this app's prepared image manifest."}];
            return NO;
        }
        maximumBytes = pinnedBytes;
    }
    NSDictionary *attributes = archive ? [archive resourceValuesForKeys:
        @[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLFileSizeKey] error:error] : nil;
    if (![attributes[NSURLIsRegularFileKey] boolValue] ||
        [attributes[NSURLIsSymbolicLinkKey] boolValue] ||
        [attributes[NSURLFileSizeKey] unsignedLongLongValue] > maximumBytes ||
        (pinnedBytes && [attributes[NSURLFileSizeKey] unsignedLongLongValue] != pinnedBytes)) {
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:pinnedBytes ? @"Select the prepared SteamOS archive with the expected size." : @"Select a regular rootfs archive up to 512 MiB."}];
        return NO;
    }
    NSURL *verifiedArchive = [NSFileManager.defaultManager.temporaryDirectory
        URLByAppendingPathComponent:[NSString stringWithFormat:@"rootfs-%@.tar.gz", NSUUID.UUID.UUIDString]];
    int input = open(archive.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat sourceStat;
    if (input >= 0 && (fstat(input, &sourceStat) < 0 || !S_ISREG(sourceStat.st_mode) || sourceStat.st_size <= 0 || (unsigned long long)sourceStat.st_size > maximumBytes)) {
        close(input); input = -1; errno = EINVAL;
    }
    int output = input < 0 ? -1 : open(verifiedArchive.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int ioError = input < 0 || output < 0 ? errno : 0;
    BOOL cancelled = NO;
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    uint8_t buffer[65536]; ssize_t count = 0; unsigned long long copied = 0;
    while (!ioError) {
        if (progress && [progress shouldCancel]) { cancelled = YES; break; }
        count = read(input, buffer, sizeof buffer);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ioError = errno; break; }
        if (!count) break;
        copied += count;
        if (copied > maximumBytes) { ioError = EFBIG; break; }
        ssize_t written = 0;
        while (written < count) {
            ssize_t n = write(output, buffer + written, count - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ioError = n < 0 ? errno : EIO; break; }
            written += n;
        }
        if (ioError) break;
        CC_SHA256_Update(&hash, buffer, (CC_LONG)count);
        [progress updateProgress:maximumBytes ? .2 * (double)copied / maximumBytes : 0 message:@"Copying and verifying the selected filesystem"];
    }
    if (input >= 0) close(input);
    if (output >= 0 && close(output) < 0 && !ioError) ioError = errno;
    uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest, &hash);
    NSMutableString *actual = [NSMutableString new];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [actual appendFormat:@"%02x", digest[i]];
    if (ioError || cancelled || (pinnedBytes && copied != pinnedBytes) || expected.length != 64 || ![actual isEqual:expected]) {
        [NSFileManager.defaultManager removeItemAtURL:verifiedArchive error:nil];
        if (error) *error = cancelled ? [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil] :
            [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:ioError ?: EINVAL
            userInfo:@{NSLocalizedDescriptionKey:ioError ? [NSString stringWithFormat:@"The filesystem could not be copied: %s", strerror(ioError)] : @"Rootfs integrity verification failed. The archive differs from its installation manifest."}];
        return NO;
    }
#endif
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *tempDestination = [NSFileManager.defaultManager.temporaryDirectory
                              URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil) {
#ifdef LINUXRUNNER
        [NSFileManager.defaultManager removeItemAtURL:verifiedArchive error:nil];
#endif
        return NO;
    }
#ifdef LINUXRUNNER
    NSURL *importArchive = verifiedArchive;
#else
    NSURL *importArchive = archive;
#endif
    BOOL imported = fakefs_import_prefix(importArchive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation, topLevel.UTF8String,
                       &fs_err, (struct progress) {(__bridge void *) progress,
#ifdef LINUXRUNNER
                           root_import_progress_callback
#else
                           root_progress_callback
#endif
                       });
#ifdef LINUXRUNNER
    [NSFileManager.defaultManager removeItemAtURL:verifiedArchive error:nil];
#endif
    if (!imported) {
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        if (error) *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey:
                                                [NSString stringWithFormat:@"%s, line %d", fs_err.message, fs_err.line]}];
        if (error && fs_err.type == ERR_CANCELLED)
            *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil];
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
#ifdef LINUXRUNNER
    NSData *installation=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:error];
    if(!installation||![installation writeToURL:[tempDestination URLByAppendingPathComponent:@"environment-install.json"] options:NSDataWritingAtomic error:error]){[NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];return NO;}
#endif
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error]) {
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

#ifdef LINUXRUNNER
static void root_stream_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    // The stream's producer reports progress; the importer only polls cancellation.
    id<ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    if (reporter && [reporter shouldCancel]) *should_cancel = true;
}
// Imports a tar stream from a descriptor (the SteamOS image reader). Integrity
// comes from the producer: it reads a SHA-256-pinned source image and verifies
// every filesystem block checksum, so there is no archive to hash here.
- (BOOL)importRootFromStream:(int)descriptor name:(NSString *)name metadata:(NSDictionary *)metadata error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    if (!validRunnerRoot(name) || [self.roots containsObject:name] || descriptor < 0) {
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:@"Invalid or duplicate environment root identifier."}];
        return NO;
    }
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    // Beside (not inside) the roots directory, on the same volume, so the
    // finished root is moved rather than copied and an interrupted import is
    // never listed as an environment. Earlier interrupted imports are removed.
    NSURL *staging = [destination.URLByDeletingLastPathComponent.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"root-imports"];
    [NSFileManager.defaultManager removeItemAtURL:staging error:nil];
    if (![NSFileManager.defaultManager createDirectoryAtURL:staging withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSURL *tempDestination = [staging URLByAppendingPathComponent:NSProcessInfo.processInfo.globallyUniqueString];
    BOOL imported = fakefs_import_fd(descriptor, tempDestination.fileSystemRepresentation, &fs_err,
                                     (struct progress) {(__bridge void *) progress, root_stream_progress_callback});
    if (!imported) {
        if (error) {
            *error = fs_err.type == ERR_CANCELLED ? [NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil] :
                [NSError errorWithDomain:fs_err.type == ERR_SQLITE ? @"SQLite" : NSPOSIXErrorDomain code:fs_err.code
                    userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"%s, line %d", fs_err.message ?: "import failed", fs_err.line]}];
        }
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    NSData *installation = [NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:error];
    if (!installation || ![installation writeToURL:[tempDestination URLByAppendingPathComponent:@"environment-install.json"] options:NSDataWritingAtomic error:error] ||
        ![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error]) {
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    void (^addRoot)(void) = ^{ [[self mutableOrderedSetValueForKey:@"roots"] addObject:name]; };
    if (!NSThread.isMainThread) dispatch_sync(dispatch_get_main_queue(), addRoot); else addRoot();
    return YES;
}
#endif

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
#ifdef LINUXRUNNER
    if (!validRunnerRoot(name) || ![self.roots containsObject:name]) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EINVAL userInfo:@{NSLocalizedDescriptionKey:@"Invalid or missing distribution."}];
        return NO;
    }
#endif
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    void (^removeRoot)(void) = ^{ [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name]; };
    if (NSThread.isMainThread) removeRoot(); else dispatch_sync(dispatch_get_main_queue(), removeRoot);
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
#ifdef LINUXRUNNER
    if (!validRunnerRoot(newName)) {
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:@"Invalid filesystem name. Use a single name without path separators."}];
        return NO;
    }
#endif
    if (name.length == 0) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([name containsString:@"/"]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't contain /"}];
        return NO;
    }
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be . or .."}];
        return NO;
    }
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot rename the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error])
        return NO;
    NSUInteger index = [self.roots indexOfObject:name];
    [[self mutableOrderedSetValueForKey:@"roots"] replaceObjectAtIndex:index withObject:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end
