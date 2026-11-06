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
#endif
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

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
#ifdef LINUXRUNNER
    return [self importVerifiedRootFromArchive:archive name:name sha256:[NSBundle.mainBundle objectForInfoDictionaryKey:@"LinuxRunnerRootfsSHA256"] metadata:@{@"distributionID":@"alpine",@"distribution":@"Alpine Linux",@"version":@"3.24.2",@"packageManager":@"apk",@"defaultShell":@"/bin/sh"} error:error progressReporter:progress];
}
- (BOOL)importVerifiedRootFromArchive:(NSURL *)archive name:(NSString *)name sha256:(NSString *)expected metadata:(NSDictionary *)metadata error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
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
    NSDictionary *attributes = archive ? [archive resourceValuesForKeys:
        @[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLFileSizeKey] error:error] : nil;
    if (![attributes[NSURLIsRegularFileKey] boolValue] ||
        [attributes[NSURLIsSymbolicLinkKey] boolValue] ||
        [attributes[NSURLFileSizeKey] unsignedLongLongValue] > 512ULL * 1024 * 1024) {
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:@"Select a regular rootfs archive up to 512 MiB."}];
        return NO;
    }
    NSURL *verifiedArchive = [NSFileManager.defaultManager.temporaryDirectory
        URLByAppendingPathComponent:[NSString stringWithFormat:@"rootfs-%@.tar.gz", NSUUID.UUID.UUIDString]];
    if (!archive || ![NSFileManager.defaultManager copyItemAtURL:archive toURL:verifiedArchive error:error])
        return NO;

    NSInputStream *stream = [NSInputStream inputStreamWithURL:verifiedArchive];
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    [stream open]; uint8_t buffer[65536]; NSInteger count;
    while ((count = [stream read:buffer maxLength:sizeof(buffer)]) > 0)
        CC_SHA256_Update(&hash, buffer, (CC_LONG)count);
    [stream close]; uint8_t digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest, &hash);
    NSMutableString *actual = [NSMutableString new];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [actual appendFormat:@"%02x", digest[i]];
    if (!stream || count < 0 || expected.length != 64 || ![actual isEqual:expected]) {
        [NSFileManager.defaultManager removeItemAtURL:verifiedArchive error:nil];
        if (error) *error = [NSError errorWithDomain:@"dev.linuxrunner.rootfs" code:EINVAL
            userInfo:@{NSLocalizedDescriptionKey:@"Rootfs integrity verification failed. The archive differs from its authenticated installation manifest."}];
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
    BOOL imported = fakefs_import(importArchive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback});
#ifdef LINUXRUNNER
    [NSFileManager.defaultManager removeItemAtURL:verifiedArchive error:nil];
#endif
    if (!imported) {
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey:
                                                [NSString stringWithFormat:@"%s, line %d", fs_err.message, fs_err.line]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
#ifdef LINUXRUNNER
    NSData *installation=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingPrettyPrinted error:error];
    if(!installation||![installation writeToURL:[tempDestination URLByAppendingPathComponent:@"environment-install.json"] options:NSDataWritingAtomic error:error]){[NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];return NO;}
#endif
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error])
        return NO;

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

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

#ifdef LINUXRUNNER
- (BOOL)stageRemovalOfRoot:(NSString *)name extraDirectories:(NSArray<NSURL *> *)directories destination:(NSURL *)destination error:(NSError **)error {
    NSAssert(NSThread.isMainThread, @"Root removal is serialized on the main thread");
    if ([name isEqual:self.defaultRoot] || ![self.roots containsObject:name]) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EBUSY userInfo:@{NSLocalizedDescriptionKey:@"Select another environment before removing this one."}];
        return NO;
    }
    NSFileManager *files = NSFileManager.defaultManager;
    if (![files createDirectoryAtURL:destination withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSArray<NSURL *> *sources = [@[[self rootUrl:name]] arrayByAddingObjectsFromArray:directories];
    NSMutableArray<NSArray<NSURL *> *> *moved = [NSMutableArray new];
    for (NSURL *source in sources) {
        if (![files attributesOfItemAtPath:source.path error:nil]) continue;
        NSURL *target = [destination URLByAppendingPathComponent:[NSString stringWithFormat:@"%lu",(unsigned long)moved.count]];
        if (![files moveItemAtURL:source toURL:target error:error]) {
            for (NSArray<NSURL *> *pair in moved.reverseObjectEnumerator) [files moveItemAtURL:pair[1] toURL:pair[0] error:nil];
            return NO;
        }
        [moved addObject:@[source,target]];
    }
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    return YES;
}
#endif

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"ios-linuxkit" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
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
