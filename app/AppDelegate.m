//
//  AppDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#include <resolv.h>
#include <arpa/inet.h>
#include <netdb.h>
#if !ISH_LINUX
#include <mach/mach.h>
#endif
#import <SystemConfiguration/SystemConfiguration.h>
#import "AboutViewController.h"
#ifdef LINUXRUNNER
#import "LinuxRunner/RuntimeBridge.h"
#import "LinuxRunner/BootLog.h"
#import "LinuxRunner/HostNameConfiguration.h"
#endif
#import "AppDelegate.h"
#import "AppGroup.h"
#import "CurrentRoot.h"
#import "ExceptionExfiltrator.h"
#import "iOSFS.h"
#import "SceneDelegate.h"
#import "PasteboardDevice.h"
#import "LocationDevice.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"
#import "TerminalViewController.h"
#import "UserPreferences.h"
#import "UIApplication+OpenURL.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "graphics/guest_gpu.h"
#if !ISH_LINUX && defined(GUEST_ARM64)
#include "DebugServer.h"
#endif
#include "kernel/native_offload.h"
#ifdef ISH_AOT_BOOTSTRAP
#include "asbestos/guest-arm64/jit.h"
#include "platform/native_fault.h"
#endif
#ifdef ISH_FFMPEG_TEST
extern void native_builtins_init(void);
#endif

#if ISH_LINUX
#import "LinuxInterop.h"
#endif

@interface AppDelegate ()

@property BOOL exiting;
@property SCNetworkReachabilityRef reachability;
#if !ISH_LINUX
@property (strong) dispatch_source_t memoryDiagnostics;
#endif

@end

#if !ISH_LINUX
static void logHostMemory(NSString *reason) {
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
        NSLog(@"HOST_MEMORY: %@ footprint=%llu resident=%llu", reason,
              (unsigned long long)info.phys_footprint, (unsigned long long)info.resident_size);
#ifdef LINUXRUNNER
        [LRRuntimeBridge logCategory:@"MEMORY" event:[NSString stringWithFormat:@"host footprint=%llu resident=%llu",
            (unsigned long long)info.phys_footprint, (unsigned long long)info.resident_size]];
#endif
    }
}

static void ios_handle_exit(struct task *task, int code) {
    // we are interested in init and in children of init
    // this is called with pids_lock as an implementation side effect, please do not cite as an example of good API design
    if (task->parent != NULL && task->parent->parent != NULL)
        return;
    // pid should be saved now since task would be freed
    pid_t pid = task->pid;
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"SIGNAL" event:[NSString stringWithFormat:@"pid=%d wait-status=%d", pid, code]];
#endif
    NSLog(@"guest process pid %d exited with code %d", pid, code);
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:ProcessExitedNotification
                                                            object:nil
                                                          userInfo:@{@"pid": @(pid),
                                                                     @"code": @(code)}];
    });
}

static void ios_handle_die(const char *msg) {
    NSString *message = [NSString stringWithFormat:@"%s: %s", __func__, msg];
    iSHExceptionHandler([[NSException alloc] initWithName:NSGenericException reason:message userInfo:nil]);
}
#elif ISH_LINUX
void ReportPanic(const char *message) {
    [NSNotificationCenter.defaultCenter postNotificationName:KernelPanicNotification object:nil userInfo:@{@"message":@(message)}];
}
#endif

static int bootError;

@implementation AppDelegate

- (int)boot {
#if !ISH_LINUX
#ifdef ISH_AOT_BOOTSTRAP
#if !defined(ISH_JIT) || !defined(ISH_JIT_NO_EMIT)
#error AOT bootstrap requires matching native/no-emitter definitions
#endif
    if (jit_aot_prepare_layout() != 0 || ish_app_native_fault_install() != 0)
        return _EINVAL;
    NSURL *identityURL = [NSBundle.mainBundle URLForResource:@"aot-build" withExtension:@"json"];
    NSData *identityData = identityURL ? [NSData dataWithContentsOfURL:identityURL] : nil;
    NSDictionary *identity = identityData ? [NSJSONSerialization JSONObjectWithData:identityData options:0 error:nil] : nil;
    if (!identity || [identity[@"executionEnabled"] boolValue] != (ISH_AOT_IMAGE_EXECUTION != 0))
        return _EINVAL;
    NSLog(@"AOT_BUILD: version=%@ build=%@ revision=%@ images=%@ enabled=%d",
          [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
          [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"],
          identity[@"sourceRevision"], identity[@"imageManifestSha256"], ISH_AOT_IMAGE_EXECUTION);
    if (setenv("ISH_JIT", ISH_AOT_IMAGE_EXECUTION ? "1" : "0", 1) != 0 ||
            setenv("ISH_AOT_FAMILY", "0", 1) != 0)
        return _ENOMEM;
#if ISH_AOT_IMAGE_EXECUTION
    NSArray *modules = identity[@"modules"];
    if ((modules.count != 3 && modules.count != 9 && modules.count != 10) || jit_aot_start((unsigned)modules.count) != 0)
        return _EINVAL;
    struct jit_layout expected;
    jit_layout_read(&expected);
    NSDictionary *contract = identity[@"contract"];
    if ([contract[@"abi"] unsignedIntValue] != expected.abi ||
            [contract[@"prologue_words"] unsignedIntValue] != expected.prologue_words ||
            [contract[@"entry_off"] unsignedIntValue] != expected.entry_off ||
            [contract[@"n_pinned"] unsignedIntValue] != expected.n_pinned)
        return _EINVAL;
#endif
    char layout[2048];
    jit_layout_describe(layout, sizeof(layout));
    NSData *layoutData = [[NSString stringWithUTF8String:layout] dataUsingEncoding:NSUTF8StringEncoding];
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSError *layoutError = nil;
    if (![layoutData writeToURL:[documents URLByAppendingPathComponent:@"aot-layout.json"] options:NSDataWritingAtomic error:&layoutError])
        NSLog(@"AOT layout evidence write failed: %@", layoutError);
    NSLog(@"AOT target layout: %s", layout);
#endif
#ifdef LINUXRUNNER
    LRBootEvent("rootfs located: validating bundled resource");
    NSURL *archiveURL = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"];
    if (!archiveURL || ![NSFileManager.defaultManager isReadableFileAtPath:archiveURL.path]) return _ENOENT;
    LRBootEvent("environment metadata: loading root catalog and verified import if needed");
    if (Roots.instance.initializationError || !Roots.instance.defaultRoot) return _EIO;
#endif
    NSURL *root = [Roots.instance rootUrl:Roots.instance.defaultRoot];
#ifdef LINUXRUNNER
    if (![NSFileManager.defaultManager fileExistsAtPath:[root URLByAppendingPathComponent:@"meta.db"].path] ||
        ![NSFileManager.defaultManager fileExistsAtPath:[root URLByAppendingPathComponent:@"data"].path]) return _ENOENT;
    LRBootEvent("runtime initialization started: mount_root");
#endif

    int err = mount_root(&fakefs, [root URLByAppendingPathComponent:@"data"].fileSystemRepresentation);
    if (err < 0)
        return err;

#ifdef LINUXRUNNER
    LRBootEvent("rootfs mounted; guest memory initialization and first process creation");
#endif
    fs_register(&iosfs);
    fs_register(&iosfs_unsafe);

    // need to do this first so that we can have a valid current for the generic_mknod calls
    err = become_first_process();
    if (err < 0)
        return err;

    err = prepare_guest_runtime_dirs();
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"FS" event:[NSString stringWithFormat:@"guest bootstrap /dev/shm mode=01777 host-page=%ld guest-page=%u result=%d", sysconf(_SC_PAGESIZE), (unsigned)PAGE_SIZE, err]];
#endif
    if (err < 0)
        return err;

#ifdef ISH_FFMPEG_TEST
    // Register built-in native handlers (fake_ffmpeg) for the ffmpeg test target.
    // This is NOT called in the standard iSH ARM64 target.
    native_builtins_init();
#endif

#ifdef LINUXRUNNER
    LRBootEvent("signal handlers installed; guest process created");
#endif
    FsInitialize();
#ifdef LINUXRUNNER
    err = [LRRuntimeBridge prepareExchange:Roots.instance.defaultRoot];
    if (err < 0) return err;
#endif

    // create some device nodes
    // this will do nothing if they already exist
    generic_mknodat(AT_PWD, "/dev/tty1", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 1));
    generic_mknodat(AT_PWD, "/dev/tty2", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 2));
    generic_mknodat(AT_PWD, "/dev/tty3", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 3));
    generic_mknodat(AT_PWD, "/dev/tty4", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 4));
    generic_mknodat(AT_PWD, "/dev/tty5", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 5));
    generic_mknodat(AT_PWD, "/dev/tty6", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 6));
    generic_mknodat(AT_PWD, "/dev/tty7", S_IFCHR|0666, dev_make(TTY_CONSOLE_MAJOR, 7));

    generic_mknodat(AT_PWD, "/dev/tty", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR));
    generic_mknodat(AT_PWD, "/dev/console", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR));
    generic_mknodat(AT_PWD, "/dev/ptmx", S_IFCHR|0666, dev_make(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR));

    generic_mknodat(AT_PWD, "/dev/null", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_NULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/zero", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_ZERO_MINOR));
    generic_mknodat(AT_PWD, "/dev/full", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_FULL_MINOR));
    generic_mknodat(AT_PWD, "/dev/random", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_RANDOM_MINOR));
    generic_mknodat(AT_PWD, "/dev/urandom", S_IFCHR|0666, dev_make(MEM_MAJOR, DEV_URANDOM_MINOR));
    
#ifdef LINUXRUNNER
    // Guest Metal device (fs/gpu.c). Every open is an isolated, quota-limited
    // context, like a Linux DRM render node, so ordinary desktop users need it:
    // before this, only root-run launchers could create it and GPU modes failed
    // or silently fell back for non-root desktop sessions. A node left 0600 by
    // older launchers is repaired here.
    generic_mknodat(AT_PWD, "/dev/linuxrunner-gpu", S_IFCHR|0666, dev_make(LR_GPU_MAJOR, 0));
    generic_setattrat(AT_PWD, "/dev/linuxrunner-gpu", (struct attr) {.type = attr_mode, .mode = S_IFCHR|0666}, false);
#endif

    generic_mkdirat(AT_PWD, "/dev/pts", 0755);
    generic_mkdirat(AT_PWD, "/mnt", 0755);
    
    // Permissions on / have been broken for a while, let's fix them
    generic_setattrat(AT_PWD, "/", (struct attr) {.type = attr_mode, .mode = 0755}, false);
    
    // Register clipboard device driver and create device node for it
    err = dyn_dev_register(&clipboard_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR);
    if (err != 0) {
        return err;
    }
    generic_mknodat(AT_PWD, "/dev/clipboard", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR));
    
    err = dyn_dev_register(&location_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_LOCATION_MINOR);
    if (err != 0)
        return err;
    generic_mknodat(AT_PWD, "/dev/location", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_LOCATION_MINOR));

    do_mount(&procfs, "proc", "/proc", "", 0);
#ifdef LINUXRUNNER
    // Software KMS device (fs/drm.c) and the sysfs entries describing it:
    // CPU rendering only. Its /dev nodes exist only while a Hyprland desktop
    // runs (session.sh), so other desktops see no DRM device.
    setenv("ISH_SOFTWARE_DRM", "1", 0);
    do_mount(&sysfs, "sysfs", "/sys", "", 0);
#endif
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);

#ifdef LINUXRUNNER
    // Shared folders from Files (iCloud Drive, other providers) may hold items
    // that are not downloaded yet ("dataless"). Let POSIX access from Linux
    // programs download them instead of failing (public I/O policy).
    LRSetDatalessMaterialization();
#endif
    iosfs_init(); // let it mount any filesystems from user defaults

    [self configureDns];
    
    exit_hook = ios_handle_exit;
    die_handler = ios_handle_die;
#if !TARGET_OS_SIMULATOR
    NSString *sockTmp = [NSTemporaryDirectory() stringByAppendingString:@"ishsock"];
    sock_tmp_prefix = strdup(sockTmp.UTF8String);
#endif
    
    tty_drivers[TTY_CONSOLE_MAJOR] = &ios_console_driver;
    set_console_device(TTY_CONSOLE_MAJOR, 1);
    err = create_stdio("/dev/console", TTY_CONSOLE_MAJOR, 1);
    if (err < 0)
        return err;

#if defined(GUEST_ARM64)
    debug_server_start(1234);
#endif

    NSArray<NSString *> *command;
    command = UserPreferences.shared.bootCommand;
#ifdef LINUXRUNNER
    if ([LRRuntimeBridge activeEnvironmentIsDebian] || [LRRuntimeBridge activeEnvironmentIsArch]) command = @[@"/bin/sh", @"/usr/lib/linuxrunner/init.sh"];
#endif
    if (command.count == 0) {
        NSLog(@"boot command is empty");
        return _EINVAL;
    }
    NSLog(@"booting guest init with command: %@", [command componentsJoinedByString:@" "]);
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp =
        "TERM=xterm-256color\0"
        "HOME=/root\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
#if defined(GUEST_ARM64)
        "PYTHONMALLOC=malloc\0"
#endif
        ;
#ifdef LINUXRUNNER
    LRBootEvent("guest init exec starting");
#endif
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
#ifdef LINUXRUNNER
    LRBootEvent("Linux runtime initialized; scheduling guest init thread");
#endif
    err = task_start(current);
    if (err < 0) {
        task_discard_unstarted(current);
        current = NULL;
        return err;
    }

#else
    // On first launch, this will trigger the import of the default root. Make sure to do this before entering the kernel, because it needs to run something on the main thread, and that would deadlock.
    [Roots instance];
    NSArray<NSString *> *args = @[];
    actuate_kernel([args componentsJoinedByString:@" "].UTF8String);
#endif
    
    return 0;
}

#if ISH_LINUX
const char *DefaultRootPath() {
    return [Roots.instance rootUrl:Roots.instance.defaultRoot].fileSystemRepresentation;
}

void SyncHostname(void) {
    async_do_in_workqueue(^{
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) < 0)
            return;
        linux_sethostname(hostname);
    });
}
#endif

#if !ISH_LINUX && defined(LINUXRUNNER)
// uname reports the configured host name (often "iPhone"), while a rootfs may
// only map "linuxrunner". sudo resolves uname's name, not /etc/hostname.
// Preserve user mappings and replace only our own marked alias. Never replace
// symlinks, hard-linked configuration or an oversized/non-text hosts file.
static void configureGuestHostAlias(void) {
    struct uname identity; do_uname(&identity);
    NSString *name=[NSString stringWithUTF8String:identity.hostname];
    struct statbuf before;
    if (generic_statat(AT_PWD,"/etc/hosts",&before,false)<0 ||
        !S_ISREG(before.mode) || before.nlink!=1 || before.size>65536) return;
    struct fd *input=generic_open("/etc/hosts",O_RDONLY_|O_NOFOLLOW_,0);
    if (IS_ERR(input)) return;
    NSMutableData *data=[NSMutableData dataWithLength:before.size];
    size_t read=0; BOOL valid=YES;
    while(read<data.length) {
        ssize_t n=input->ops->read(input,(char *)data.mutableBytes+read,data.length-read);
        if(n<=0) {valid=NO;break;} read+=n;
    }
    fd_close(input);
    if(!valid) return;
    NSString *old=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];
    NSString *updated=LRHostsForName(old,name);
    if(!updated || [updated isEqual:old]) return;
    NSString *path=[NSString stringWithFormat:@"/etc/.linuxrunner-hosts-%@",NSUUID.UUID.UUIDString];
    struct fd *output=generic_open(path.UTF8String,O_WRONLY_|O_CREAT_|O_EXCL_|O_NOFOLLOW_,before.mode&0777);
    if(IS_ERR(output)) return;
    NSData *bytes=[updated dataUsingEncoding:NSUTF8StringEncoding];size_t written=0;
    while(written<bytes.length) {
        ssize_t n=output->ops->write(output,(const char *)bytes.bytes+written,bytes.length-written);
        if(n<=0) break; written+=n;
    }
    int sync=output->ops->fsync?output->ops->fsync(output):0;
    fd_close(output);
    int owner=generic_setattrat(AT_PWD,path.UTF8String,make_attr(uid,before.uid),false);
    if(!owner) owner=generic_setattrat(AT_PWD,path.UTF8String,make_attr(gid,before.gid),false);
    struct statbuf now;
    BOOL same=generic_statat(AT_PWD,"/etc/hosts",&now,false)==0 &&
        now.inode==before.inode && now.size==before.size && now.mtime==before.mtime && now.mtime_nsec==before.mtime_nsec;
    if(written==bytes.length && sync==0 && owner==0 && same &&
        generic_renameat(AT_PWD,path.UTF8String,AT_PWD,"/etc/hosts")==0)
        [LRRuntimeBridge logCategory:@"NETWORK" event:@"Local hostname alias updated; existing hosts mappings preserved"];
    else generic_unlinkat(AT_PWD,path.UTF8String);
}
#endif

- (void)configureDns {
#if !ISH_LINUX
#ifdef LINUXRUNNER
    // Reachability fires even in native-only mode. Never borrow a missing init.
    lock(&pids_lock); BOOL hasInit = pid_get_task(1) != NULL; unlock(&pids_lock);
    if (!hasInit) return;
#endif
    struct __res_state res;
    BOOL resolverInitialized = EXIT_SUCCESS == res_ninit(&res);
    if (!resolverInitialized) {
        NSLog(@"[DNS] res_ninit failed, falling back to public resolver");
    }
    NSMutableString *resolvConf = [NSMutableString new];
    if (resolverInitialized && res.dnsrch[0] != NULL) {
        [resolvConf appendString:@"search"];
        for (int i = 0; res.dnsrch[i] != NULL; i++) {
            [resolvConf appendFormat:@" %s", res.dnsrch[i]];
        }
        [resolvConf appendString:@"\n"];
    }
    union res_sockaddr_union servers[NI_MAXSERV];
    int serversFound = resolverInitialized && res.nscount > 0 ? res_getservers(&res, servers, NI_MAXSERV) : 0;
    char address[NI_MAXHOST];
    int nameserversWritten = 0;
    for (int i = 0; i < serversFound; i ++) {
        union res_sockaddr_union s = servers[i];
        if (s.sin.sin_len == 0)
            continue;
        int err = getnameinfo((struct sockaddr *) &s.sin, s.sin.sin_len,
                              address, sizeof(address),
                              NULL, 0, NI_NUMERICHOST);
        if (err != 0 || address[0] == '\0')
            continue;
        [resolvConf appendFormat:@"nameserver %s\n", address];
        nameserversWritten++;
    }
    if (resolverInitialized)
        res_nclose(&res);

    if (nameserversWritten == 0) {
        NSLog(@"[DNS] no system nameservers found, falling back to 1.1.1.1");
        [resolvConf appendString:@"nameserver 1.1.1.1\n"];
    }
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"NETWORK" event:[NSString stringWithFormat:@"DNS configured; system resolver count=%d", nameserversWritten]];
#endif
    
    current = pid_get_task(1);
#ifdef LINUXRUNNER
    configureGuestHostAlias();
#endif
    struct fd *fd = generic_open("/etc/resolv.conf", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0666);
    if (!IS_ERR(fd)) {
        fd->ops->write(fd, resolvConf.UTF8String, [resolvConf lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        fd_close(fd);
    }
#endif
}

+ (int)bootError {
    return bootError;
}

- (BOOL)application:(UIApplication *)application willFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey,id> *)launchOptions {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults boolForKey:@"hail mary"]) {
        [defaults removeObjectForKey:kPreferenceBootCommandKey];
        [defaults removeObjectForKey:kPreferenceLaunchCommandKey];
        [defaults setBool:NO forKey:@"hail mary"];
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])
        return YES;

#ifdef LINUXRUNNER
    LRBootEvent("application delegate initialized");
    if (![LRRuntimeBridge autoStart]) { LRBootEvent("runtime auto start OFF: native workspace available before boot"); return YES; }
    [self startLinuxRuntime];
#else
    CFAbsoluteTime bootStart = CFAbsoluteTimeGetCurrent();
    bootError = [self boot];
#ifdef LINUXRUNNER
    [LRRuntimeBridge recordBoot:Roots.instance.defaultRoot error:bootError];
    [LRRuntimeBridge logCategory:@"EXEC" event:[NSString stringWithFormat:@"boot to task scheduling ms=%.1f", (CFAbsoluteTimeGetCurrent() - bootStart) * 1000]];
#endif

#endif
#if ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillEnterForegroundNotification object:UIApplication.sharedApplication queue:nil usingBlock:^(NSNotification * _Nonnull note) {
        SyncHostname();
    }];
    SyncHostname();
#endif

    return YES;
}

#ifdef LINUXRUNNER
- (int)startLinuxRuntime {
    if (![LRRuntimeBridge beginBoot]) return bootError;
    // Before the first guest block: the JIT starts only if iOS already allows it.
    [LRRuntimeBridge prepareExecutionBackendAtLaunch];
    CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
    bootError = [self boot];
    if (bootError == 0) [LRRuntimeBridge applySharedFolderOwnership];
    [LRRuntimeBridge recordBoot:Roots.instance.defaultRoot ?: @"default" error:bootError];
    [LRRuntimeBridge logCategory:@"EXEC" event:[NSString stringWithFormat:@"boot to task scheduling ms=%.1f", (CFAbsoluteTimeGetCurrent() - start) * 1000]];
    return bootError;
}
#endif

void NetworkReachabilityCallback(SCNetworkReachabilityRef target, SCNetworkReachabilityFlags flags, void *info) {
    AppDelegate *self = (__bridge AppDelegate *) info;
    [self configureDns];
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // get the network permissions popup to appear on chinese devices
    [[NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:@"http://captive.apple.com"]] resume];

    if ([NSUserDefaults.standardUserDefaults boolForKey:@"FASTLANE_SNAPSHOT"])
        [UIView setAnimationsEnabled:NO];

#if !ISH_LINUX
    if (strcmp(getenv("ISH_MEMORY_DIAGNOSTICS") ?: "", "1") == 0) {
        self.memoryDiagnostics = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(self.memoryDiagnostics, DISPATCH_TIME_NOW,
                2 * NSEC_PER_SEC, NSEC_PER_SEC / 4);
        dispatch_source_set_event_handler(self.memoryDiagnostics, ^{
            logHostMemory(@"sample");
        });
        dispatch_resume(self.memoryDiagnostics);
    }
    NSString *ishVersion = [NSString stringWithFormat:@"ios-linuxkit %@ (%@)",
                         [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                         [NSBundle.mainBundle objectForInfoDictionaryKey:(NSString *) kCFBundleVersionKey]];
    extern const char *proc_ish_version;
    proc_ish_version = strdup(ishVersion.UTF8String);
#ifdef ISH_AOT_BOOTSTRAP
    NSDictionary *identity = [NSJSONSerialization JSONObjectWithData:
        [NSData dataWithContentsOfURL:[NSBundle.mainBundle URLForResource:@"aot-build" withExtension:@"json"]]
        options:0 error:nil];
    NSString *aotVersion = [ishVersion stringByAppendingFormat:@" AOT=%d images=%@ revision=%@",
        ISH_AOT_IMAGE_EXECUTION, identity[@"imageManifestSha256"], identity[@"sourceRevision"]];
    free((void *)proc_ish_version);
    proc_ish_version = strdup(aotVersion.UTF8String);
#endif
    // this defaults key is set when taking app store screenshots
    extern const char *uname_hostname_override;
    NSString *hostnameOverride = UserPreferences.shared._hostnameOverride;
    if (@available(iOS 16.0, *)) { // Hostname obfuscation is in effect
        hostnameOverride = hostnameOverride ? hostnameOverride : UserPreferences.shared.hostnameOverride;
    }
    if (hostnameOverride) {
        uname_hostname_override = strdup(hostnameOverride.UTF8String);
    }
#ifdef LINUXRUNNER
    // Automatic boot runs before this preference is applied. Refresh the
    // rootfs alias after choosing the final uname hostname as well.
    [self configureDns];
#endif
#endif
    
    [UserPreferences.shared observe:@[@"shouldDisableDimming"] options:NSKeyValueObservingOptionInitial
                              owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
#ifdef LINUXRUNNER
            [LRRuntimeBridge refreshAwake];   // the awake manager owns idleTimerDisabled (this preference and active operations)
#else
            UIApplication.sharedApplication.idleTimerDisabled = UserPreferences.shared.shouldDisableDimming;
#endif
        });
    }];
    
    // This code is IPv4 and IPv6 aware: see https://developer.apple.com/library/archive/samplecode/Reachability/Listings/ReadMe_md.html
    struct sockaddr_in address = {
        .sin_len = sizeof(address),
        .sin_family = AF_INET,
    };
    self.reachability = SCNetworkReachabilityCreateWithAddress(kCFAllocatorDefault, (struct sockaddr *) &address);
    SCNetworkReachabilityContext context = {
        .info = (__bridge void *) self,
    };
    SCNetworkReachabilitySetCallback(self.reachability, NetworkReachabilityCallback, &context);
    SCNetworkReachabilityScheduleWithRunLoop(self.reachability, CFRunLoopGetMain(), kCFRunLoopCommonModes);

    if (self.window != nil) {
        // For iOS <13, where the app delegate owns the window instead of the scene
        #ifdef LINUXRUNNER
        if (NO) { // LinuxRunner has no inherited recovery settings screen
#else
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
#endif
            UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
            AboutViewController *avc = (AboutViewController *) vc.topViewController;
            avc.recoveryMode = YES;
            self.window.rootViewController = vc;
            return YES;
        }
        TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
        currentTerminalViewController = vc;
        [vc startNewSession];
    }
    return YES;
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions API_AVAILABLE(ios(13.0)) {
    for (UISceneSession *sceneSession in sceneSessions) {
        NSString *terminalUUID = sceneSession.stateRestorationActivity.userInfo[@"TerminalUUID"];
        [[Terminal terminalWithUUID:[[NSUUID alloc] initWithUUIDString:terminalUUID]] destroy];
    }
}

- (void)dealloc {
#if !ISH_LINUX
    if (self.memoryDiagnostics)
        dispatch_source_cancel(self.memoryDiagnostics);
#endif
    if (self.reachability != NULL) {
        SCNetworkReachabilityUnscheduleFromRunLoop(self.reachability, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(self.reachability);
    }
}

- (void)exitApp {
    NSLog(@"APP_SHUTDOWN: user-requested exit");
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Close LinuxRunner"
        message:@"Close the app from the app switcher, then reopen it to start the selected environment. Files are preserved."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [currentTerminalViewController presentViewController:alert animated:YES completion:nil];
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    if (self.exiting) {
        NSLog(@"APP_SHUTDOWN: exiting after background transition");
        exit(0);
    }
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)application {
#if !ISH_LINUX
    logHostMemory(@"memory warning");
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"MEMORY" event:@"memory warning received"];
#endif
#endif
}

@end

#if !ISH_LINUX
NSString *const ProcessExitedNotification = @"ProcessExitedNotification";
#else
NSString *const KernelPanicNotification = @"KernelPanicNotification";
#endif
