#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import "app/Roots.h"
#include <assert.h>
#include <locale.h>
#include <unistd.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

// Isolated native importer test. No app container or user preferences are used.
static NSURL *fixture;
NSURL *ContainerURL(void) { return fixture; }
void LRBootEvent(const char *message) { (void)message; }
static NSString *digestOf(NSData *data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *text = [NSMutableString new];
    for (NSUInteger i = 0; i < sizeof digest; i++) [text appendFormat:@"%02x", digest[i]];
    return text;
}

@interface TestProgress : NSObject <ProgressReporter>
@property BOOL cancelled;
@property BOOL cancelDuringImport;
@end
@implementation TestProgress
- (BOOL)shouldCancel { return self.cancelled; }
- (void)updateProgress:(double)value message:(NSString *)message {
    (void)message;
    if (self.cancelDuringImport && value >= .2) self.cancelled = YES;
}
@end

int main(void) { @autoreleasepool {
    assert(setlocale(LC_ALL, "C"));
    locale_t initialLocale = uselocale((locale_t)0);
    NSFileManager *files = NSFileManager.defaultManager;
    fixture = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    NSURL *content = [fixture URLByAppendingPathComponent:@"content/etc"];
    assert([files createDirectoryAtURL:content withIntermediateDirectories:YES attributes:nil error:nil]);
    NSData *original = [@"ID=steamos\n" dataUsingEncoding:NSUTF8StringEncoding];
    assert([original writeToURL:[content URLByAppendingPathComponent:@"os-release"] atomically:YES]);
    NSString *unicodeName = @"Főtanúsítvány-日本語.crt";
    assert([original writeToURL:[content URLByAppendingPathComponent:unicodeName] atomically:YES]);
    assert([files createSymbolicLinkAtPath:[content URLByAppendingPathComponent:@"unicode-link"].path withDestinationPath:unicodeName error:nil]);
    // Foundation supplies the filesystem spelling (NFD on macOS) to symlink().
    // Compare the imported link with those actual source bytes, not an NFC
    // spelling that was never present in the archive fixture.
    char sourceLink[4096];
    ssize_t sourceLinkLength = readlink([content URLByAppendingPathComponent:@"unicode-link"].fileSystemRepresentation, sourceLink, sizeof sourceLink);
    assert(sourceLinkLength > 0);
    NSData *expectedLink = [NSData dataWithBytes:sourceLink length:(NSUInteger)sourceLinkLength];
    NSURL *archive = [fixture URLByAppendingPathComponent:@"rootfs.tar.gz"];
    NSTask *tar = [NSTask new]; tar.executableURL = [NSURL fileURLWithPath:@"/usr/bin/tar"];
    tar.arguments = @[@"--format=pax", @"-czf", archive.path, @"-C", content.URLByDeletingLastPathComponent.path, @"etc"];
    tar.environment = @{@"LC_ALL":@"en_US.UTF-8", @"LANG":@"en_US.UTF-8"};
    assert([tar launchAndReturnError:nil]); [tar waitUntilExit]; assert(tar.terminationStatus == 0);
    NSData *bytes = [NSData dataWithContentsOfURL:archive];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, digest);
    NSMutableString *sha = [NSMutableString new];
    for (NSUInteger i = 0; i < sizeof digest; i++) [sha appendFormat:@"%02x", digest[i]];
    NSString *source = @"3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676";
    NSURL *support = [NSBundle.mainBundle.resourceURL URLByAppendingPathComponent:@"SteamOSSupport.bundle"];
    assert([files createDirectoryAtURL:support withIntermediateDirectories:YES attributes:nil error:nil]);
    NSDictionary *manifest = @{@"schema":@1, @"architecture":@"arm64", @"archiveSHA256":sha,
        @"sourceImageSHA256":source, @"archiveBytes":@(bytes.length)};
    assert([[NSJSONSerialization dataWithJSONObject:manifest options:0 error:nil]
        writeToURL:[support URLByAppendingPathComponent:@"manifest.json"] atomically:YES]);
    assert([NSBundle.mainBundle URLForResource:@"SteamOSSupport" withExtension:@"bundle"]);
    // Avoid the app's boot-time default-root creation in this fixture.
    Roots *roots = [Roots alloc];
    [roots setValue:[NSMutableOrderedSet new] forKey:@"roots"];
    NSDictionary *alpine = @{@"distributionID":@"alpine"};
    NSDictionary *steam = @{@"distributionID":@"steamos", @"sourceImageSHA256":source};
    NSError *error = nil;
    NSTask *invalid = [NSTask new]; invalid.executableURL = [NSURL fileURLWithPath:@"/usr/bin/python3"];
    invalid.arguments = @[@"-c", @"import gzip,pathlib,sys; p=pathlib.Path(sys.argv[1]); raw=gzip.decompress(p.read_bytes()); assert b'\\xcc\\x8b' in raw; p.with_name('bad-utf8.tar.gz').write_bytes(gzip.compress(raw.replace(b'\\xcc\\x8b',b'\\xff\\x8b',1)))", archive.path];
    assert([invalid launchAndReturnError:nil]); [invalid waitUntilExit]; assert(!invalid.terminationStatus);
    NSURL *badUTF8 = [fixture URLByAppendingPathComponent:@"bad-utf8.tar.gz"];
    assert(![roots importVerifiedRootFromArchive:badUTF8 name:@"bad-utf8" sha256:digestOf([NSData dataWithContentsOfURL:badUTF8]) metadata:alpine error:&error progressReporter:nil]);
    assert(error && ![files fileExistsAtPath:[roots rootUrl:@"bad-utf8"].path]);
    // Match the fixture's hash so this exercises codec failure during import,
    // independently of the source-integrity check that precedes extraction.
    NSData *truncatedGzip = [bytes subdataWithRange:NSMakeRange(0, 16)];
    NSURL *badGzip = [fixture URLByAppendingPathComponent:@"bad-gzip.tar.gz"];
    assert([truncatedGzip writeToURL:badGzip atomically:YES]);
    error = nil;
    assert(![roots importVerifiedRootFromArchive:badGzip name:@"bad-gzip" sha256:digestOf(truncatedGzip) metadata:alpine error:&error progressReporter:nil]);
    assert(error && ![files fileExistsAtPath:[roots rootUrl:@"bad-gzip"].path]);
    error = nil;
    assert(![roots importVerifiedRootFromArchive:archive name:@"wrong-hash" sha256:[@"b" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0] metadata:alpine error:&error progressReporter:nil]);
    assert(error && ![files fileExistsAtPath:[roots rootUrl:@"wrong-hash"].path]);
    TestProgress *progress = [TestProgress new]; progress.cancelled = YES; error = nil;
    assert(![roots importVerifiedRootFromArchive:archive name:@"cancel-copy" sha256:sha metadata:alpine error:&error progressReporter:progress]);
    assert(error.code == NSUserCancelledError && ![files fileExistsAtPath:[roots rootUrl:@"cancel-copy"].path]);
    progress = [TestProgress new]; progress.cancelDuringImport = YES; error = nil;
    assert(![roots importVerifiedRootFromArchive:archive name:@"cancel-import" sha256:sha metadata:alpine error:&error progressReporter:progress]);
    assert(error.code == NSUserCancelledError && ![files fileExistsAtPath:[roots rootUrl:@"cancel-import"].path]);
    NSURL *link = [fixture URLByAppendingPathComponent:@"symlink.tar.gz"];
    assert([files createSymbolicLinkAtURL:link withDestinationURL:archive error:nil]);
    assert(![roots importVerifiedRootFromArchive:link name:@"link" sha256:sha metadata:alpine error:&error progressReporter:nil]);
    assert((![roots importVerifiedRootFromArchive:archive name:@"wrong-source" sha256:sha metadata:@{@"distributionID":@"steamos", @"sourceImageSHA256":@"wrong"} error:&error progressReporter:nil]));
    error = nil;
    if (![roots importVerifiedRootFromArchive:archive name:@"steam" sha256:sha metadata:steam error:&error progressReporter:nil]) {
        fprintf(stderr, "STEAMOS_ROOT_IMPORT: FAIL %s\n", error.localizedDescription.UTF8String);
        return 1;
    }
    assert(!error);
    NSData *imported = [NSData dataWithContentsOfURL:[[roots rootUrl:@"steam"] URLByAppendingPathComponent:@"data/etc/os-release"]];
    assert([imported isEqual:original] && [roots.roots containsObject:@"steam"]);
    NSData *unicode = [NSData dataWithContentsOfURL:[[roots rootUrl:@"steam"] URLByAppendingPathComponent:[@"data/etc/" stringByAppendingString:unicodeName]]];
    assert([unicode isEqual:original]);
    NSData *linkBytes = [NSData dataWithContentsOfURL:[[roots rootUrl:@"steam"] URLByAppendingPathComponent:@"data/etc/unicode-link"]];
    assert([linkBytes isEqual:expectedLink]);
    assert(uselocale((locale_t)0) == initialLocale && !strcmp(setlocale(LC_CTYPE, NULL), "C"));
    assert([[NSData dataWithContentsOfURL:archive] isEqual:bytes]);
    assert([files removeItemAtURL:fixture error:nil]);
    puts("STEAMOS_ROOT_IMPORT: PASS (pins, cancellation, UTF-8 names in C locale, malformed UTF-8 and truncated gzip rejection)");
} return 0; }
