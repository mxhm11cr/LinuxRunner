#import "RuntimeBridge.h"
#include "LinuxAccountPolicy.h"
#import "GuestMetalBackend.h"
#import "FileTransfer.h"
#import "FileArchive.h"
#import "DiagnosticLogTail.h"
#import "DebianBootstrap.h"
#import "BootLog.h"
#import "GUIViewController.h"
#import "LinuxRunner-Swift.h"
#import "UserPreferences.h"
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#ifdef LINUXRUNNER_JIT_BUILD
#include "asbestos/guest-arm64/jit.h"
#endif
#include <os/log.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "fs/netlink.h"
#include <mach/mach.h>
#include <archive.h>
#include <archive_entry.h>
#import "../Roots.h"
#import "../AppGroup.h"
#import "../AppDelegate.h"
#if LINUXRUNNER_VIRGL_BUILD
// Host renderer for the virtio-gpu device (upstream virglrenderer + ANGLE, linked statically).
#include "../../graphics/virgl_host.c"
#endif
#include "../../graphics/virgl_backend.h"
#include "fs/drm.h"
static BOOL LRVirglActive;
static NSString *LRVirglStatus;   // GPU start result (renderer or failure), nil when not started
#import "../iOSFS.h"
#include "PasswordHash.h"
@class TerminalViewController;
static void LRWaitForProcess(TerminalViewController *process, NSTimeInterval timeout, void (^done)(BOOL ended));
static int LRTransferStaging(void);
@interface TerminalViewController (LRAccountProcess)
@property (readonly) int sessionPid;
@property (nonatomic) Terminal *sessionTerminal;
@end
@interface LRRuntimeBridge (LRSupportInstall)
+ (void)runSupportInstall:(NSArray<NSString *> *)arguments title:(NSString *)title progress:(void (^)(NSString *))progress completion:(void (^)(NSError * _Nullable))completion;
+ (void)runDesktopProfile:(NSString *)mode progress:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion;
@end
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include "kernel/task.h"
#include "kernel/mm.h"
#include "kernel/resource.h"
#include "kernel/signal.h"
#include "kernel/futex.h"
#include "fs/proc.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/path.h"
#include "fs/fd.h"

// Persistent startup trace, initialized in main before UIKit delegates/roots.
// Never called by an OS signal handler. Fatal signals remain Apple's crash reports.
static NSURL *bootLogDirectory;
static NSTimeInterval runtimeStartedAt;
static atomic_int diagnosticLevel=2, diagnosticPID=-1, diagnosticSyscall=-1;
static atomic_bool diagnosticErrorsOnly=true;
static int bootLogFD = -1;
static pthread_mutex_t bootLogLock = PTHREAD_MUTEX_INITIALIZER;
static char lastMilestone[2048] = "not initialized";
struct LRBootSnapshot { _Atomic uint64_t version, pid, pc, sp, syscall, opcode; };
static struct LRBootSnapshot *bootSnapshot;
static atomic_flag snapshotWriter = ATOMIC_FLAG_INIT;
__attribute__((constructor)) static void LRProcessStarted(void) {
    const char message[] = "[BOOT] process started (C constructor)\n";
    write(STDERR_FILENO, message, sizeof(message) - 1);
}
void LRBootEvent(const char *message) {
    if (!message) return;
    int saved = errno;
    pthread_mutex_lock(&bootLogLock);
    snprintf(lastMilestone, sizeof(lastMilestone), "%s", message);
    char line[2300]; struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    int n = snprintf(line, sizeof(line), "%lld.%03ld [BOOT] %s\n", (long long)now.tv_sec, now.tv_nsec / 1000000, lastMilestone);
    if (n > 0) {
        size_t count = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        write(STDERR_FILENO, line, count);
        if (bootLogFD >= 0) { struct stat st; if (fstat(bootLogFD, &st) == 0 && st.st_size < 1024 * 1024) { write(bootLogFD, line, count); fsync(bootLogFD); } }
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s", lastMilestone);
    }
    pthread_mutex_unlock(&bootLogLock);
    @autoreleasepool {
        NSString *text=[NSString stringWithUTF8String:message]?:@"unreadable event";
        int severity=([text containsString:@"fault"]||[text containsString:@"FAIL"]||[text containsString:@"error="])?0:2;
        NSString *category=@"RUNTIME";
        if ([text hasPrefix:@"["]) { NSRange close=[text rangeOfString:@"]"]; if(close.location!=NSNotFound && close.location<32) category=[text substringWithRange:NSMakeRange(1,close.location-1)]; }
        [LRDiagnosticCenter record:category severity:severity message:text pid:0 tid:0];
    }
    errno = saved;
}
void LRBootSnapshot(int pid, uint64_t pc, uint64_t sp, uint64_t syscall, uint64_t opcode) {
    if (!bootSnapshot || atomic_flag_test_and_set_explicit(&snapshotWriter, memory_order_acquire)) return;
    atomic_store(&bootSnapshot->pid, pid); atomic_store(&bootSnapshot->pc, pc);
    atomic_store(&bootSnapshot->sp, sp); atomic_store(&bootSnapshot->syscall, syscall);
    if (opcode) atomic_store(&bootSnapshot->opcode, opcode);
    atomic_flag_clear_explicit(&snapshotWriter, memory_order_release);
}
void LRStartupInitialize(void) {
    // No installer can survive process termination; verified package caches do.
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"InstallerActive"];
    [LRDiagnosticCenter configure:[[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"LinuxRunnerDiagnostics" isDirectory:YES]];
    [LRGuestCrashCenter configure:[[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"LinuxRunnerDiagnostics/application-crashes" isDirectory:YES]];
    bootLogDirectory = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"LinuxRunnerLogs" isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:bootLogDirectory withIntermediateDirectories:YES attributes:nil error:&error]) { LRBootEvent("persistent startup log unavailable"); return; }
    for (NSString *name in @[@"startup.log", @"runtime-snapshot.bin"]) {
        NSURL *source = [bootLogDirectory URLByAppendingPathComponent:name];
        NSURL *previous = [bootLogDirectory URLByAppendingPathComponent:[@"previous-" stringByAppendingString:name]];
        [NSFileManager.defaultManager removeItemAtURL:previous error:nil];
        [NSFileManager.defaultManager moveItemAtURL:source toURL:previous error:nil];
    }
    bootLogFD = open([bootLogDirectory URLByAppendingPathComponent:@"startup.log"].fileSystemRepresentation, O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW, 0600);
    int fd = open([bootLogDirectory URLByAppendingPathComponent:@"runtime-snapshot.bin"].fileSystemRepresentation, O_RDWR|O_CREAT|O_NOFOLLOW, 0600);
    if (fd >= 0 && ftruncate(fd, sizeof(struct LRBootSnapshot)) == 0) {
        void *map = mmap(NULL, sizeof(struct LRBootSnapshot), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) { bootSnapshot = map; atomic_store(&bootSnapshot->version, 1); }
    }
    if (fd >= 0) close(fd);
    LRBootEvent("process entered main; persistent log initialized");
    // Raise the host soft limit once (shared by all guest processes; the guest
    // RLIMIT_NOFILE is separate) and record the inherited and resulting values.
    extern const char *host_nofile_describe(char *buf, size_t size);
    char limits[360] = "HOST_RESOURCES: ";
    host_nofile_describe(limits + strlen(limits), sizeof limits - strlen(limits));
    LRBootEvent(limits);
}
static void LRObserveSyscall(int pid, uint64_t pc, uint64_t sp, unsigned syscall) { LRBootSnapshot(pid, pc, sp, syscall, 0); }
static void LRObserveMemory(const char *operation, uint64_t address, uint64_t length, int protection, int flags, int error) {
    char message[256]; snprintf(message, sizeof(message), "MEMORY %s address=0x%llx length=%llu prot=%d flags=%d errno=%d", operation, (unsigned long long)address, (unsigned long long)length, protection, flags, error);
    LRBootEvent(message);
}
extern void (*runtime_syscall_observer)(int, uint64_t, uint64_t, unsigned);
extern void (*runtime_guest_fault)(int, int, const char *, uint64_t, uint64_t, int, uint64_t, uint32_t);
extern void (*runtime_fault_context)(const char *);
static void LRGuestFault(int pid,int ppid,const char *comm,uint64_t pc,uint64_t sp,int signal,uint64_t address,uint32_t instruction) {
    char line[300];snprintf(line,sizeof line,"guest fault pid=%d ppid=%d comm=%.16s signal=%d pc=%#llx sp=%#llx address=%#llx instruction=%#x",pid,ppid,comm,signal,(unsigned long long)pc,(unsigned long long)sp,(unsigned long long)address,instruction);LRBootEvent(line);
    @autoreleasepool {
        char executable[MAX_PATH] = "";
        if(current && current->mm && current->mm->exefile) generic_getpath(current->mm->exefile,executable);
        uint64_t lr = current ? current->cpu.regs[30] : 0;
        NSString *root = LRRuntimeBridge.activeRootName ?: @"unknown";
        NSString *decoded = (instruction & 0xffe0001f) == 0xd4200000 ? [NSString stringWithFormat:@"BRK #%u",(instruction>>5)&65535] : instruction == 0x2ea07842 ? @"SQNEG v2.2s, v2.2s" : @"raw instruction; full disassembly unavailable";
        [LRGuestCrashCenter begin:@{@"application":[NSString stringWithUTF8String:comm]?:@"unknown",@"executable":[NSString stringWithUTF8String:executable]?:@"unknown",@"pid":@(current?current->tgid:pid).stringValue,@"tid":@(pid).stringValue,@"ppid":@(ppid).stringValue,@"signal":@(signal).stringValue,@"signal_name":signal==4?@"SIGILL":signal==5?@"SIGTRAP":signal==6?@"SIGABRT":signal==7?@"SIGBUS":signal==11?@"SIGSEGV":@"none",@"pc":[NSString stringWithFormat:@"%#llx",(unsigned long long)pc],@"lr":[NSString stringWithFormat:@"%#llx",(unsigned long long)lr],@"sp":[NSString stringWithFormat:@"%#llx",(unsigned long long)sp],@"address":[NSString stringWithFormat:@"%#llx",(unsigned long long)address],@"instruction":[NSString stringWithFormat:@"%#x",instruction],@"disassembly":decoded,@"rootfs":root,@"version":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]?:@"unknown",@"build":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]?:@"unknown",@"source_commit":[NSBundle.mainBundle objectForInfoDictionaryKey:@"LinuxRunnerSourceCommit"]?:@"unknown",@"environment":@"iOS ARM64 interpreter; 4 KB guest pages"}];
    }
}
extern void (*runtime_guest_output)(int,int,int,const char *);
static void LRGuestOutput(int tid,int tgid,int fd,const char *text) {
    @autoreleasepool {
        NSString *safe=[LRDiagnosticCenter crashOutput:[NSString stringWithUTF8String:text]?:@"unreadable output"];
        [LRGuestCrashCenter append:[NSString stringWithFormat:@"FAULT_%@ tid=%d tgid=%d %@",fd==2?@"STDERR":@"STDOUT",tid,tgid,safe] tid:tid];
    }
}
extern void (*runtime_guest_terminated)(int,int,int);
static void LRGuestTerminated(int tid,int signal,int exitCode) { @autoreleasepool { [LRGuestCrashCenter finish:tid signal:signal exitCode:exitCode]; } }
static void LRFaultContext(const char *line) {
    LRBootEvent(line);
    @autoreleasepool { [LRGuestCrashCenter append:[NSString stringWithUTF8String:line]?:@"" tid:current?current->pid:0]; }
}
extern atomic_bool runtime_syscall_timing_enabled;
extern atomic_bool runtime_futex_pi_trace_enabled;
extern void (*runtime_syscall_result_observer)(int, int, unsigned, const uint64_t *, int64_t, uint64_t);
static const char *LRCallName(unsigned n) {
    switch(n){case 22:return "epoll_pwait";case 25:return "fcntl";case 52:return "fchmod";case 54:return "fchownat";case 452:return "fchmodat2";case 56:return "openat";case 57:return "close";case 63:return "read";case 64:return "write";case 73:return "ppoll";case 93:return "exit";case 94:return "exit_group";case 95:return "waitid";case 98:return "futex";case 129:return "kill";case 134:return "rt_sigaction";case 135:return "rt_sigprocmask";case 198:return "socket";case 199:return "socketpair";case 208:return "sendto";case 211:return "sendmsg";case 212:return "recvmsg";case 214:return "brk";case 215:return "munmap";case 220:return "clone";case 221:return "execve";case 222:return "mmap";case 226:return "mprotect";case 279:return "memfd_create";case 435:return "clone3";default:return "aarch64-syscall";}
}
static void LRObserveCompletedCall(int tid,int pid,unsigned number,const uint64_t *args,int64_t result,uint64_t duration) {
    int level=atomic_load(&diagnosticLevel),p=atomic_load(&diagnosticPID),n=atomic_load(&diagnosticSyscall);
    if((p>=0&&p!=pid&&p!=tid)||(n>=0&&n!=(int)number))return;
    BOOL lifecycle=number==93||number==94||number==220||number==221||number==435||number==95||number==129;
    if(level<3&&!lifecycle&&result!=-38&&result!=-12&&result!=-24&&result!=-23)return;
    if(level<4&&atomic_load(&diagnosticErrorsOnly)&&!lifecycle&&result>=0)return;
    @autoreleasepool {
        NSString *message=[NSString stringWithFormat:@"name=%s number=%u result=%lld errno=%lld duration_ns=%llu duration_measured=%d args=%llx,%llx,%llx,%llx,%llx,%llx",LRCallName(number),number,(long long)result,(long long)(result<0&&result>=-4095?-result:0),(unsigned long long)duration,duration!=0,(unsigned long long)args[0],(unsigned long long)args[1],(unsigned long long)args[2],(unsigned long long)args[3],(unsigned long long)args[4],(unsigned long long)args[5]];
        [LRDiagnosticCenter record:lifecycle?@"PROCESS":@"SYSCALL" severity:result<0?1:2 message:message pid:pid tid:tid];
    }
}
extern void (*runtime_memory_failure)(const char *, uint64_t, uint64_t, int, int, int);

extern int fakefs_bind_mount(const char *, const char *, bool);
extern int fakefs_bind_mount_set_owner(const char *, uint32_t, uint32_t);
static BOOL bootAttempted;
static NSString *bootedRoot;
static int bootResult;
static NSMutableArray<NSString *> *events;
extern void (*runtime_instruction_failure)(int, uint64_t, uint32_t);
static void instructionFailure(int pid, uint64_t pc, uint32_t instruction) {
    LRBootSnapshot(pid, pc, current ? current->cpu.sp : 0, current ? current->syscall_restart_num : 0, instruction);
    @autoreleasepool {
        [LRRuntimeBridge logCategory:@"LOADER" event:[NSString stringWithFormat:
            @"unsupported instruction pid=%d pc=0x%llx encoding=0x%08x", pid, (unsigned long long)pc, instruction]];
    }
}

static NSError *lrError(int code, NSString *message) {
    return [NSError errorWithDomain:@"dev.linuxrunner.runtime" code:code
                          userInfo:@{NSLocalizedDescriptionKey:message}];
}
static BOOL validRoot(NSString *name) {
    return name.length && ![name isEqual:@"."] && ![name isEqual:@".."] &&
        [name rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"/\\\0"]].location == NSNotFound;
}

@interface LRDeviceDiagnosticsController : UIViewController
@property UILabel *details;
@property UIButton *startButton;
@property UIButton *managerButton;
@property UISwitch *autoStartSwitch;
@end

@interface LRInstallProgress : NSObject <ProgressReporter>
@property(copy) void (^progress)(double,NSString *);
@property(copy) BOOL (^cancel)(void);
@end
@implementation LRInstallProgress
- (void)updateProgress:(double)value message:(NSString *)message { if(self.progress)self.progress(value,message); }
- (BOOL)shouldCancel { return self.cancel&&self.cancel(); }
@end
static int LRPackageProgress(void *context,size_t value,size_t total) { LRInstallProgress *reporter=(__bridge LRInstallProgress *)context;[reporter updateProgress:(double)value/MAX(total,1) message:@"Assembling verified packages"];return reporter.shouldCancel; }
static int LRPackageCancel(void *context) { return ((__bridge LRInstallProgress *)context).shouldCancel; }
static void LRPackageDiagnostic(void *context,int severity,const char *message) {
    (void)context;
    @autoreleasepool { [LRDiagnosticCenter installer:severity message:[NSString stringWithUTF8String:message]?:@"bootstrap diagnostic encoding failure"]; }
}

@interface LRTransferControl ()
@property BOOL stopped;
@property uint64_t transferred,total;
@end
@implementation LRTransferControl
- (void)cancel { @synchronized(self){_stopped=YES;} }
- (NSDictionary *)snapshot { @synchronized(self){return @{@"cancelled":@(_stopped),@"bytes":@(_transferred),@"total":@(_total)};} }
- (void)recordBytes:(uint64_t)bytes total:(uint64_t)total { @synchronized(self){_transferred=bytes;_total=total;} }
@end
static bool LRTransferCancelled(void *context){return [((__bridge LRTransferControl *)context).snapshot[@"cancelled"] boolValue];}
static void LRTransferProgress(void *context,uint64_t bytes,uint64_t total){[(__bridge LRTransferControl *)context recordBytes:bytes total:total];}
static struct lr_transfer_control LRTransferCallbacks(LRTransferControl *control){return (struct lr_transfer_control){LRTransferCancelled,LRTransferProgress,(__bridge void *)control};}

@implementation LRRuntimeBridge
#if TARGET_OS_SIMULATOR
+ (NSDictionary *)networkABIContract {
    struct header {uint32_t len;uint16_t type,flags;uint32_t seq,pid;};
    struct {struct header h;uint8_t family,pad[3];} req={.h={17,18,0x301,41,0}};
    NSMutableDictionary *result=[NSMutableDictionary new];BOOL ok=YES;
    char *buffer=malloc(NETLINK_SNAPSHOT_SIZE);
    if(!buffer)return @{@"status":@"FAIL",@"reason":@"allocation"};
    for(NSNumber *type in @[@18,@22,@26]) {
        req.h.type=type.unsignedShortValue;
        int n=netlink_snapshot(&req,sizeof(req),73,buffer,NETLINK_SNAPSHOT_SIZE);
        unsigned messages=0;BOOL done=NO;int error=0;
        for(size_t at=0;n>0&&at<(size_t)n;) {
            struct header h;if((size_t)n-at<sizeof(h)){error=-1;break;}
            memcpy(&h,buffer+at,sizeof(h));
            if(h.len<sizeof(h)||h.len>(size_t)n-at||h.seq!=41||h.pid!=73){error=-1;break;}
            if(h.type==2&&h.len>=sizeof(h)+4)memcpy(&error,buffer+at+sizeof(h),4);
            else if(h.type==3)done=YES;else messages++;
            at+=((size_t)h.len+3)&~(size_t)3;
        }
        BOOL passed=n>0&&done&&!error&&(type.intValue==26||messages>0);
        result[type.stringValue]=@{@"status":passed?@"PASS":@"FAIL",@"messages":@(messages),@"linux_error":@(n<0?n:error)};ok=ok&&passed;
    }
    free(buffer);result[@"status"]=ok?@"PASS":@"FAIL";
    result[@"scope"]=@"Simulator sandbox, real host snapshots encoded as Linux link/address/route replies; no physical Wi-Fi transition";
    return result;
}
#endif
#if TARGET_OS_SIMULATOR
+ (void)runExternalDisplayContract:(void (^)(NSDictionary *))completion{[LRGUIViewController runExternalDisplayContract:completion];}
#endif

static UINavigationController *desktopNavigation;
static NSString *desktopRoot;
+ (UIViewController *)desktopController {
    NSString *root=self.activeRootName;
    BOOL reuse=desktopNavigation&&[desktopRoot isEqual:root];
    if(!reuse){desktopRoot=[root copy];desktopNavigation=[[UINavigationController alloc] initWithRootViewController:[[LRGUIViewController alloc] initDesktop]];}
    if(!(reuse&&desktopNavigation.presentingViewController))[self desktopTrace:@"02 view" result:@"info" detail:[NSString stringWithFormat:@"desktop controller %@ root=%@",reuse?@"reused":@"created",root]];
    return desktopNavigation;
}
+ (UIViewController *)desktopControllerIfCreated { return [desktopRoot isEqual:self.activeRootName] ? desktopNavigation : nil; }
+ (NSDictionary *)desktopSessionSummary {
    if(!desktopNavigation||![desktopRoot isEqual:self.activeRootName])return @{@"state":@"Stopped",@"alive":@NO,@"connected":@NO};
    return [(LRGUIViewController *)desktopNavigation.viewControllers.firstObject desktopSessionSummary];
}
+ (NSDictionary *)graphicsSummary { return [LRGUIViewController latestGraphicsSummary]; }
+ (void)stopDesktopSession {
    if([desktopRoot isEqual:self.activeRootName])[(LRGUIViewController *)desktopNavigation.viewControllers.firstObject stopDesktopSession];
}
+ (UIViewController *)desktopControllerWithConfiguration:(NSDictionary *)configuration {
    UINavigationController *nav=(id)[self desktopController];
    [(LRGUIViewController *)nav.topViewController launchDesktopApplication:configuration];return nav;
}
+ (UIViewController *)guiController { return [[UINavigationController alloc] initWithRootViewController:[LRGUIViewController new]]; }
+ (UIViewController *)guiControllerWithConfiguration:(NSDictionary *)configuration {return [[UINavigationController alloc] initWithRootViewController:[[LRGUIViewController alloc] initWithLaunchConfiguration:configuration]];}
+ (BOOL)autoStart {
    id configured = [NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerRuntimeAutoStart"];
    return configured ? [configured boolValue] : ![[NSBundle.mainBundle objectForInfoDictionaryKey:@"LinuxRunnerDiagnostic"] boolValue];
}
+ (BOOL)canStartRuntime { return !bootAttempted; }
+ (int)startRuntime {
    NSAssert(NSThread.isMainThread, @"Boot is serialized on the main thread");
    return [(AppDelegate *)UIApplication.sharedApplication.delegate startLinuxRuntime];
}
+ (BOOL)beginBoot {
    NSAssert(NSThread.isMainThread, @"Boot is serialized on the main thread");
    if (bootAttempted) return NO; // Partial initialization cannot safely restart.
    bootAttempted = YES;
    // Select once before any guest execution/decoded blocks. A live toggle
    // would mix incompatible gadget operand streams; preferences apply only
    // after a full process relaunch. On by default from build 884 (validated on
    // the Mac by the ABI suites, app and browser tests); an explicit Settings
    // choice is kept, and turning it off restores the previous dispatch path.
    extern void arm64_memory_pc_fusion_set_enabled_from_env(const char *);
    id fusionSetting = [NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunner.experimentalMemoryPCFusion"];
    BOOL memoryFusion = fusionSetting ? [fusionSetting boolValue] : YES;
    arm64_memory_pc_fusion_set_enabled_from_env(memoryFusion ? "1" : "0");
    LRBootEvent(memoryFusion ? (fusionSetting ? "[RUNTIME] Asbestos memory dispatch fusion enabled (Settings)" : "[RUNTIME] Asbestos memory dispatch fusion enabled (default)") : "[RUNTIME] Asbestos previous memory dispatch selected in Settings");
    LRInstallGuestMetalBackend();
    LRBootEvent("[GUEST_GPU] experimental EGL/GLES subset backend registered; existing Mesa libraries unchanged");
#if LINUXRUNNER_VIRGL_BUILD
    // Opt-in GPU acceleration: guest Mesa's virgl driver on the Apple GPU. If
    // ANGLE/Metal cannot start, the virtio-gpu device stays absent and
    // everything renders in software as before.
    if ([LRRuntimeBridge.gpuMode isEqual:@"metal"]) {
        const struct lr_virgl_backend *virgl = lr_virgl_host_start();
        lr_virgl_install(virgl);
        LRVirglActive = virgl != NULL;
        LRVirglStatus = [NSString stringWithUTF8String:lr_virgl_host_status()];
        if (virgl) setenv("ISH_VIRGL", "1", 1);
        LRBootEvent([[NSString stringWithFormat:virgl ? @"[GPU] virtio-gpu device active: virglrenderer on %@" : @"[GPU] Metal GPU unavailable (%@); software rendering", LRVirglStatus] UTF8String]);
    }
#endif
    // All anonymous guest shared-memory backing stays inside this sandbox.
    NSString *temporary = NSTemporaryDirectory();
    if (temporary.length) setenv("TMPDIR", temporary.fileSystemRepresentation, 1);
    LRBootEvent("guest shared-memory backing configured in application temporary directory");
    id networkView=[NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerNetworkInterfaceView"];
    setenv("ISH_NETLINK_STUB",(!networkView||[networkView boolValue])?"1":"0",1);
    LRBootEvent("read-only host network interface compatibility configured; XNU retains network ownership");
    runtime_syscall_observer = LRObserveSyscall;
    runtime_guest_fault = LRGuestFault;
    runtime_fault_context = LRFaultContext;
    runtime_guest_terminated = LRGuestTerminated;
    runtime_guest_output = LRGuestOutput;
    extern volatile bool g_trace_syscalls, g_trace_faults;
    g_trace_syscalls = g_trace_faults = [NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerVerboseRuntime"];
    runtime_memory_failure = LRObserveMemory;
    runtimeStartedAt=NSProcessInfo.processInfo.systemUptime;
    atomic_store(&diagnosticLevel,[NSUserDefaults.standardUserDefaults objectForKey:@"DiagnosticLevel"]?[NSUserDefaults.standardUserDefaults integerForKey:@"DiagnosticLevel"]:2);
    atomic_store(&runtime_syscall_timing_enabled,atomic_load(&diagnosticLevel)>=3);
    runtime_syscall_result_observer=LRObserveCompletedCall;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(thermalStateChanged:) name:NSProcessInfoThermalStateDidChangeNotification object:nil];
    LRBootEvent("manual/automatic boot requested; optional numeric observers installed");
    return YES;
}
+ (UIViewController *)diagnosticsController { return [LRDeviceDiagnosticsController new]; }
+ (NSDictionary<NSString *,NSString *> *)deviceDiagnostics {
    extern bool arm64_memory_pc_fusion_is_enabled(void);
    NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSString *name = [NSUserDefaults.standardUserDefaults stringForKey:@"Default Root"] ?: @"default";
    NSURL *root = [[ContainerURL() URLByAppendingPathComponent:@"roots"] URLByAppendingPathComponent:name];
    NSURL *archive = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"];
    pthread_mutex_lock(&bootLogLock); NSString *last = [NSString stringWithUTF8String:lastMilestone]; pthread_mutex_unlock(&bootLogLock);
    return @{
        @"Device architecture": @"arm64", @"iOS": UIDevice.currentDevice.systemVersion,
        @"Asbestos memory dispatch": !bootAttempted ? @"not started" : arm64_memory_pc_fusion_is_enabled() ? @"experimental fusion (Mac-tested, device-unverified)" : @"stable dispatch",
        @"App bundle path": NSBundle.mainBundle.bundlePath,
        @"Application Support": support.path ?: @"unavailable", @"Runtime container": ContainerURL().path ?: @"unavailable",
        @"Bundled rootfs": archive.path ?: @"missing", @"Bundled rootfs exists": archive && [NSFileManager.defaultManager isReadableFileAtPath:archive.path] ? @"yes" : @"no",
        @"Writable rootfs path": root.path ?: @"unavailable", @"Writable rootfs exists": [NSFileManager.defaultManager fileExistsAtPath:[root URLByAppendingPathComponent:@"meta.db"].path] ? @"yes" : @"no (not imported yet)",
        @"Runtime state": self.runtimeStatus, @"Last startup milestone": last ?: @"unknown",
        @"Last runtime error": bootResult < 0 ? [NSString stringWithFormat:@"boot errno=%d; relaunch to retry", bootResult] : LRGuestCrashCenter.latestSummary,
        @"Host startup error": bootResult < 0 ? @(bootResult).stringValue : @"none recorded",
        @"Latest guest application failure": LRGuestCrashCenter.latestSummary,
        @"Startup logs": bootLogDirectory.path ?: @"stderr only", @"Host page size": [NSString stringWithFormat:@"%ld", sysconf(_SC_PAGESIZE)]
    };
}
+ (NSURL *)managedStorageURL { return ContainerURL(); }
+ (NSArray *)diagnosticEntries { return [LRDiagnosticCenter entries]; }
+ (NSArray<NSDictionary *> *)desktopDiagnosticEntries {
    NSMutableArray *records=[NSMutableArray new];
    NSArray *native=self.diagnosticEntries;
    for(NSDictionary *entry in [native subarrayWithRange:NSMakeRange(native.count>512?native.count-512:0,MIN(native.count,512))]){
        NSMutableDictionary *safe=[entry mutableCopy];
        safe[@"message"]=[self redactDiagnosticText:entry[@"message"]?:@"" application:NO];
        [records addObject:safe];
    }
    NSURL *base=[[self exchangeURL:self.activeRootName] URLByAppendingPathComponent:@".linuxrunner/gui"];
    // Every component below the exchange is app-managed. Refuse replaced links.
    for(NSURL *folder in @[[base URLByDeletingLastPathComponent],base]){
        NSNumber *link=nil;[folder getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil];if(link.boolValue)return records;
    }
    NSArray<NSURL *> *children=[NSFileManager.defaultManager contentsOfDirectoryAtURL:base includingPropertiesForKeys:@[NSURLIsSymbolicLinkKey,NSURLIsDirectoryKey,NSURLContentModificationDateKey] options:0 error:nil];
    NSMutableArray<NSURL *> *sessions=[NSMutableArray new];
    for(NSURL *child in children){NSNumber *link=nil,*directory=nil;[child getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil];[child getResourceValue:&directory forKey:NSURLIsDirectoryKey error:nil];if([child.lastPathComponent hasPrefix:@"session-"]&&directory.boolValue&&!link.boolValue)[sessions addObject:child];}
    [sessions sortUsingComparator:^NSComparisonResult(NSURL *a,NSURL *b){NSDate *ad=nil,*bd=nil;[a getResourceValue:&ad forKey:NSURLContentModificationDateKey error:nil];[b getResourceValue:&bd forKey:NSURLContentModificationDateKey error:nil];return [(bd?:NSDate.distantPast) compare:ad?:NSDate.distantPast];}];
    NSDictionary *components=@{@"session.log":@"Session",@"xvfb.log":@"X11",@"display.log":@"Display",@"bus.log":@"D-Bus",@"wm.log":@"Window Manager",@"panel.log":@"Panel",@"desktop.log":@"Desktop",@"plasma.log":@"Plasma",@"application.log":@"Application",@"focus.log":@"Input",@"window.log":@"Windows",@"desktop-state":@"Status"};
    for(NSURL *session in [sessions subarrayWithRange:NSMakeRange(0,MIN(sessions.count,3))]) {
        for(NSString *name in [components.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            NSURL *url=[session URLByAppendingPathComponent:name];
            NSString *text=LRDiagnosticTailString(LRReadDiagnosticTail(url,64*1024));if(!text)continue;
            NSDate *modified=nil;[url getResourceValue:&modified forKey:NSURLContentModificationDateKey error:nil];
            text=[self redactDiagnosticText:text application:YES];
            NSArray *lines=[text componentsSeparatedByString:@"\n"];
            NSUInteger first=lines.count>120?lines.count-120:0;
            for(NSUInteger i=first;i<lines.count&&records.count<1536;i++){
                NSString *line=lines[i];if(!line.length||[line hasPrefix:@"[non-diagnostic"]||[line hasPrefix:@"[application output"])continue;
                NSString *lower=line.lowercaseString,*level=([lower containsString:@"error"]||[lower containsString:@"fatal"]||[lower containsString:@"abort"])?@"ERROR":[lower containsString:@"warning"]?@"WARNING":@"INFO";
                NSString *component=[lower containsString:@"kwin"]?@"KWin":components[name];
                [records addObject:@{@"timestamp":@(modified?modified.timeIntervalSince1970:0),@"file_time":@YES,@"category":component,@"level":level,@"message":[line substringToIndex:MIN(line.length,1024)],@"source":name}];
            }
        }
    }
    return records;
}

+ (NSString *)redactDiagnosticText:(NSString *)text application:(BOOL)application { return [LRDiagnosticCenter redact:text application:application]; }
+ (void)configureDiagnostics:(int)level process:(int)pid syscall:(int)number errorsOnly:(BOOL)errorsOnly {
    atomic_store(&diagnosticLevel,level);atomic_store(&diagnosticPID,pid);atomic_store(&diagnosticSyscall,number);atomic_store(&diagnosticErrorsOnly,errorsOnly);
    atomic_store(&runtime_syscall_timing_enabled,level>=3);
    atomic_store(&runtime_futex_pi_trace_enabled,level>=3 && (number<0||number==98));
    [LRDiagnosticCenter setLevel:level];
}
static NSString *LRThermalState(void) {
    switch(NSProcessInfo.processInfo.thermalState){case NSProcessInfoThermalStateNominal:return @"Nominal";case NSProcessInfoThermalStateFair:return @"Fair";case NSProcessInfoThermalStateSerious:return @"Serious";case NSProcessInfoThermalStateCritical:return @"Critical";default:return @"Unknown";}
}
+ (void)thermalStateChanged:(NSNotification *)note {
    [self logCategory:@"MEMORY" event:[NSString stringWithFormat:@"host thermal state changed: %@ (iOS category; no temperature measurement)",LRThermalState()]];
}
+ (NSArray *)taskSnapshot {
    struct row { int pid, ppid, uid, threads; char state; char name[17]; uint64_t cpu_us; struct mm *mm; };
    struct row *rows = calloc(MAX_PID, sizeof *rows); int count = 0;
    if (!rows) return @[];
    lock(&pids_lock);
    for (int i = 1; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (!task || task->pid != task->tgid || task->exiting) continue;
        struct row *r = &rows[count++];
        r->pid = task->pid; r->ppid = task_parent_pid(task); r->uid = (int) task->uid;
        snprintf(r->name, sizeof r->name, "%s", task->comm);
        bool running = false; struct task *thread;
        lock(&task->group->lock);
        list_for_each_entry(&task->group->threads, thread, group_links) { r->threads++; if (!thread->blocking) running = true; }
        bool stopped = task->group->stopped;
        unlock(&task->group->lock);
        r->state = task->zombie ? 'Z' : stopped ? 'T' : running ? 'R' : 'S';
        struct rusage_ usage = rusage_get_group_locked(task->group);
        r->cpu_us = (uint64_t) usage.utime.sec * 1000000 + usage.utime.usec + (uint64_t) usage.stime.sec * 1000000 + usage.stime.usec;
        lock(&task->general_lock);
        r->mm = task->mm; if (r->mm) mm_retain(r->mm);
        unlock(&task->general_lock);
    }
    unlock(&pids_lock);
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:count];
    for (int i = 0; i < count; i++) {
        struct row *r = &rows[i];
        unsigned long mapped = 0, shared = 0, anonymous = 0;
        if (r->mm) { mem_page_counts(&r->mm->mem, &mapped, &shared, &anonymous); mm_release(r->mm); }   // outside pids_lock
        [result addObject:@{@"pid": @(r->pid), @"ppid": @(r->ppid), @"uid": @(r->uid), @"name": @(r->name), @"state": [NSString stringWithFormat:@"%c", r->state],
            @"threads": @(r->threads), @"cpu_ms": @(r->cpu_us / 1000), @"mapped_kb": @(mapped * (PAGE_SIZE / 1024)),
            @"anonymous_kb": @(anonymous * (PAGE_SIZE / 1024)), @"shared_kb": @(shared * (PAGE_SIZE / 1024))}];
    }
    free(rows);
    return result;
}
+ (uint64_t)appFootprintBytes {
    task_vm_info_data_t info = {0}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t) &info, &count) == KERN_SUCCESS ? info.phys_footprint : 0;
}
+ (BOOL)terminateProcess:(int)pid force:(BOOL)force {
    if (pid <= 1) return NO;   // init (the session's root) is never terminated from here
    lock(&pids_lock);
    struct task *task = pid_get_task(pid);
    BOOL found = task && task->pid == task->tgid && !task->zombie;
    if (found) send_signal(task, force ? SIGKILL_ : SIGTERM_, SIGINFO_NIL);
    unlock(&pids_lock);
    [self logCategory:@"PROCESS" event:[NSString stringWithFormat:@"Task Manager sent %@ to pid %d (%@)", force ? @"SIGKILL" : @"SIGTERM", pid, found ? @"delivered" : @"no such process"]];
    return found;
}
+ (NSDictionary *)healthSummary {
    int processes=0,threads=0;
    lock(&pids_lock);
    for(int i=1;i<MAX_PID;i++){struct task *task=pid_get_task(i);if(task&&!task->exiting&&!task->zombie){threads++;if(task->pid==task->tgid)processes++;}}
    unlock(&pids_lock);
    mach_task_basic_info_data_t info={0};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    BOOL haveMemory=task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&count)==KERN_SUCCESS;
    return @{@"runtime":self.runtimeStatus,@"source_commit":[NSBundle.mainBundle objectForInfoDictionaryKey:@"LinuxRunnerSourceCommit"]?:@"unavailable",@"distribution_root":self.activeRootName?:@"none",@"architecture":@"aarch64",@"backend":@"ARM64 interpreter",@"jit_backend_active":@NO,@"host_page_size":@(getpagesize()),@"guest_page_size":@4096,@"host_resident_bytes":haveMemory?@(info.resident_size):@0,@"memory_measured":@(haveMemory),@"host_thermal_state":LRThermalState(),@"low_power_mode":@(NSProcessInfo.processInfo.lowPowerModeEnabled),@"guest_processes":@(processes),@"guest_threads":@(threads),@"runtime_uptime_seconds":@(runtimeStartedAt?NSProcessInfo.processInfo.systemUptime-runtimeStartedAt:0),@"thread_state_limit":@"Counts are live tasks; no claim of precise sleeping/blocked state"};
}
+ (NSURL *)catalogURL { return [ContainerURL() URLByAppendingPathComponent:@"environments.json"]; }
+ (NSArray<NSString *> *)rootNames { return Roots.instance.roots.array; }
+ (NSString *)activeRootName { return bootedRoot ?: Roots.instance.defaultRoot; }
+ (NSString *)nextRootName { return Roots.instance.defaultRoot; }
+ (void)setNextRootName:(NSString *)name {
    if (validRoot(name) && [Roots.instance.roots containsObject:name]) Roots.instance.defaultRoot = name;
}
+ (NSURL *)rootURL:(NSString *)name {
    NSParameterAssert(validRoot(name));
    return [Roots.instance rootUrl:name];
}
+ (NSURL *)exchangeURL:(NSString *)name {
    NSParameterAssert(validRoot(name));
    return [[ContainerURL() URLByAppendingPathComponent:@"exchanges" isDirectory:YES]
            URLByAppendingPathComponent:name isDirectory:YES];
}
+ (BOOL)isRunning {
    if (!bootAttempted || bootResult < 0) return NO;
    lock(&pids_lock);
    BOOL running = pid_get_task(1) != NULL;
    unlock(&pids_lock);
    return running;
}
+ (NSString *)runtimeStatus {
    if (!bootAttempted) return @"Not started";
    return bootResult < 0 ? [NSString stringWithFormat:@"Boot failed (%d)", bootResult] :
        ([self isRunning] ? @"Running" : @"Stopped — relaunch required");
}
+ (void)recordBoot:(NSString *)root error:(int)error {
    bootedRoot = [root copy]; bootResult = error;
    [self logCategory:@"EXEC" event:[NSString stringWithFormat:@"boot result=%d backend=asbestos jit=false", error]];
}
+ (int)prepareExchange:(NSString *)root {
    runtime_instruction_failure = instructionFailure;
    NSError *error;
    NSURL *exchange = [self exchangeURL:root];
    if (![NSFileManager.defaultManager createDirectoryAtURL:exchange withIntermediateDirectories:YES attributes:nil error:&error]) return _EIO;
    for (const char **path = (const char *[]){"/home", "/home/mobile", NULL}; *path; path++) {
        int result = generic_mkdirat(AT_PWD, *path, 0755);
        if (result < 0 && result != _EEXIST) return result;
    }
    int result = fakefs_bind_mount("/home/mobile/Downloads", exchange.fileSystemRepresentation, false);
    [self logCategory:@"FS" event:[NSString stringWithFormat:@"exchange mount result=%d", result]];
    return result;
}
+ (NSDictionary *)probeJITPermissions {
    // Public VM APIs only. A successful protection change is NOT proof that
    // generated code may execute; this shipping binary has no JIT backend.
    size_t size=getpagesize();void *page=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    int allocationError=page==MAP_FAILED?errno:0,protectionError=0;
    BOOL permitted=NO;if(page!=MAP_FAILED){permitted=mprotect(page,size,PROT_READ|PROT_EXEC)==0;if(!permitted)protectionError=errno;munmap(page,size);}
    NSDictionary *result=@{@"backend_available":@NO,@"backend_active":@NO,@"executable_protection_change":@(permitted),@"allocation_errno":@(allocationError),@"protection_errno":@(protectionError),@"generated_code_executed":@NO,@"capability_verified":@NO,@"reason":@"Interpreter-only build. Public VM permission probe does not establish generated-code execution or translator activation."};
    [self logCategory:@"JIT" event:[NSString stringWithFormat:@"permission_probe=%d allocation_errno=%d protection_errno=%d backend_available=0 capability_verified=0",permitted,allocationError,protectionError]];return result;
}
// ---- Execution backend -----------------------------------------------------
// The interpreter always runs. In the JIT-capable variant the translator
// starts once jit_ios_acquire() succeeds; nothing maps executable memory
// before that, and the mode the user picked is never reported as active.
static NSString *const LRExecutionModeKey=@"LinuxRunnerExecutionMode";
static NSString *const LRJITAttemptKey=@"LinuxRunnerJITAttemptInProgress";
static NSString *const LRJITCrashedPathsKey=@"LinuxRunnerJITCrashedPaths";
static BOOL LRJITActive;
static NSString *LRJITDetail=@"Interpreter";
static NSString *LRJITLastFailure;   // reason of the last refused or failed activation, nil if none
+ (BOOL)virglCompiledIn {
#if LINUXRUNNER_VIRGL_BUILD
    return YES;
#else
    return NO;
#endif
}
+ (BOOL)virglActive { return LRVirglActive; }
+ (void)setDesktopDisplayWidth:(NSUInteger)width height:(NSUInteger)height { drm_set_display_size((unsigned) width, (unsigned) height); }
// Settings › Graphics: "automatic", "metal" or "software". Automatic renders in
// software until Metal is validated on iPhone hardware; the former on/off
// setting maps to metal/software.
+ (NSString *)gpuMode {
    NSUserDefaults *d=NSUserDefaults.standardUserDefaults;NSString *mode=[d stringForKey:@"LinuxRunnerGraphics.gpuMode"];
    if([@[@"automatic",@"metal",@"software"] containsObject:mode])return mode;
    if([d objectForKey:@"LinuxRunnerGraphics.virgl"])return [d boolForKey:@"LinuxRunnerGraphics.virgl"]?@"metal":@"software";
    return @"automatic";
}
+ (void)setGPUMode:(NSString *)mode {
    if(![@[@"automatic",@"metal",@"software"] containsObject:mode])mode=@"automatic";
    [NSUserDefaults.standardUserDefaults setObject:mode forKey:@"LinuxRunnerGraphics.gpuMode"];
    [NSUserDefaults.standardUserDefaults removeObjectForKey:@"LinuxRunnerGraphics.virgl"];
}
// The renderer this run actually has, never the requested one.
+ (NSDictionary *)gpuStatus {
    NSString *mode=self.gpuMode,*active,*detail;
    if(!self.virglCompiledIn){active=@"Software";detail=@"This build has no Metal GPU device";}
    else if(LRVirglActive){active=@"Metal GPU (virgl) · Experimental";detail=[NSString stringWithFormat:@"virtio-gpu device active · %@",LRVirglStatus?:@"ANGLE/Metal"];}
    else if(LRVirglStatus.length){active=@"Software (fallback)";detail=[NSString stringWithFormat:@"Metal GPU did not start: %@",LRVirglStatus];}
    else if([mode isEqual:@"metal"]){active=@"Software";detail=@"Metal GPU selected · takes effect after LinuxRunner restarts";}
    else if([mode isEqual:@"automatic"]){active=@"Software";detail=@"Automatic uses software rendering until Metal is validated on iPhone";}
    else {active=@"Software";detail=@"Software rendering selected";}
    return @{@"mode":mode,@"active":active,@"detail":detail,@"device":@(LRVirglActive),@"restartNeeded":@([mode isEqual:@"metal"]!=LRVirglActive&&!LRVirglStatus.length)};
}
+ (BOOL)jitCompiledIn {
#ifdef LINUXRUNNER_JIT_BUILD
    return YES;
#else
    return NO;
#endif
}
+ (NSString *)executionMode {
    NSString *mode=[NSUserDefaults.standardUserDefaults stringForKey:LRExecutionModeKey];
    // Not chosen yet: a JIT-capable build uses the JIT once iOS allows it
    // (StikDebug), the interpreter otherwise; an explicit choice is kept.
    if(!mode)return self.jitCompiledIn?@"automatic":@"interpreter";
    return [@[@"automatic",@"jit"] containsObject:mode]?mode:@"interpreter";
}
+ (void)setExecutionMode:(NSString *)mode {
    [NSUserDefaults.standardUserDefaults setObject:[@[@"automatic",@"jit"] containsObject:mode]?mode:@"interpreter" forKey:LRExecutionModeKey];
}
+ (BOOL)debuggerAttached {
    struct kinfo_proc info={0};size_t size=sizeof info;int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};
    return sysctl(mib,4,&info,&size,NULL,0)==0&&(info.kp_proc.p_flag&P_TRACED);
}
+ (BOOL)requiresPreparedJITRegion { return NSProcessInfo.processInfo.operatingSystemVersion.majorVersion>=26; }
+ (NSDictionary *)executionBackendStatus {
    return @{@"compiled":@(self.jitCompiledIn),@"mode":self.executionMode,@"active":LRJITActive?@"jit":@"interpreter",@"detail":LRJITDetail?:@"",@"debugger":@(self.debuggerAttached),@"txm":@(self.requiresPreparedJITRegion)};
}
+ (BOOL)activateJIT:(NSString **)detail {
    NSString *(^done)(BOOL,NSString *)=^NSString *(BOOL ok,NSString *why){LRJITActive=LRJITActive||ok;LRJITDetail=why;[self logCategory:@"JIT" event:[NSString stringWithFormat:@"activate ok=%d %@",ok,why]];return why;};
#ifdef LINUXRUNNER_JIT_BUILD
    if(LRJITActive){if(detail)*detail=LRJITDetail;return YES;}
    NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
    NSArray *crashed=[defaults stringArrayForKey:LRJITCrashedPathsKey]?:@[];
    BOOL traced=self.debuggerAttached,txmDevice=self.requiresPreparedJITRegion;
    NSMutableArray *paths=[NSMutableArray array];
    // Classic dual mapping: iOS's own mprotect check decides; harmless when refused.
    if(![crashed containsObject:@"classic"])[paths addObject:@"classic"];
    // TXM: only while a debugger is attached (a brk without StikDebug's script kills the app).
    if(txmDevice&&traced&&![crashed containsObject:@"txm"])[paths addObject:@"txm"];
    NSString *why=traced||!txmDevice?@"No usable JIT path":@"Attach LinuxRunner with StikDebug first (iOS 26 and later need its script)";
    for(NSString *path in paths){
        [defaults setObject:path forKey:LRJITAttemptKey];[defaults synchronize];
        char text[256]={0};int rc=jit_ios_acquire([path isEqual:@"txm"],text,sizeof text);
        [defaults removeObjectForKey:LRJITAttemptKey];[defaults synchronize];
        why=[NSString stringWithFormat:@"%@: %s",[path isEqual:@"txm"]?@"TXM":@"Classic",text];
        if(rc==0){if(detail)*detail=done(YES,[@"JIT active · " stringByAppendingString:why]);return YES;}
    }
    LRJITLastFailure=why;
    if(detail)*detail=done(NO,why);return NO;
#else
    if(detail)*detail=done(NO,@"This build is interpreter-only; install the JIT build to use the JIT.");return NO;
#endif
}
// What actually runs, from runtime facts only: build variant, chosen mode,
// debugger state, the verified code region (a generated stub returned the
// expected value) and the translator's counters (native segments installed
// in guest block code streams, which the dispatcher then runs).
+ (NSDictionary *)jitStatusReport {
    NSMutableDictionary *r=[NSMutableDictionary dictionaryWithDictionary:@{@"compiled":@(self.jitCompiledIn),@"mode":self.executionMode,@"debugger":@(self.debuggerAttached),@"txm":@(self.requiresPreparedJITRegion),@"os":NSProcessInfo.processInfo.operatingSystemVersionString}];
    NSArray *crashed=[NSUserDefaults.standardUserDefaults stringArrayForKey:LRJITCrashedPathsKey]?:@[];
    NSString *state,*detail;
#ifdef LINUXRUNNER_JIT_BUILD
    uint64_t blocks=0,segments=0,bytes=0;jit_counters(&blocks,&segments,&bytes);
    r[@"blocks"]=@(blocks);r[@"segments"]=@(segments);r[@"code_kb"]=@(bytes/1024);
    if(LRJITActive&&segments>0){state=@"JIT Active";detail=[NSString stringWithFormat:@"%@ · %llu guest blocks translated, %llu native segments installed, %llu KB generated code",LRJITDetail,blocks,segments,bytes/1024];}
    else if(LRJITActive){state=@"JIT Initializing";detail=[NSString stringWithFormat:@"%@ · code region verified (test code returned the expected value); no guest code translated yet — start or use Linux",LRJITDetail];}
    else if(crashed.count&&!LRJITLastFailure){state=@"JIT Failed";detail=[NSString stringWithFormat:@"A previous activation (%@) ended LinuxRunner; that method is disabled. Interpreter fallback active.",[crashed componentsJoinedByString:@", "]];}
    else if([self.executionMode isEqual:@"interpreter"]){state=@"Interpreter Active";detail=self.debuggerAttached?@"A debugger is attached: choose Automatic or JIT to use it":@"Interpreter selected";}
    else if(LRJITLastFailure){state=@"JIT Unavailable";detail=[LRJITLastFailure stringByAppendingString:@" · interpreter fallback active"];}
    else if(self.debuggerAttached){state=@"JIT Available";detail=@"Debugger attached · tap Activate JIT Now";}
    else {state=@"Waiting for JIT Authorization";detail=self.requiresPreparedJITRegion?@"Debugger attachment not detected · attach with StikDebug (its universal script) and return here":@"Executable memory not yet allowed · enable JIT for LinuxRunner in StikDebug and return here";}
#else
    state=@"Interpreter Active";detail=@"This build is interpreter-only";
#endif
    r[@"state"]=state;r[@"detail"]=detail;return r;
}
+ (void)retryJITWhenAuthorized {
    // StikDebug usually grants JIT while LinuxRunner is running: try again on
    // each return to the foreground. The classic path is iOS's own mprotect
    // check (harmless when refused); TXM only runs while a debugger is attached.
    if(!self.jitCompiledIn||LRJITActive||[self.executionMode isEqual:@"interpreter"])return;
    if(self.requiresPreparedJITRegion&&!self.debuggerAttached){[self logCategory:@"JIT" event:@"foreground check: no debugger attached"];return;}
    NSString *why=nil;[self activateJIT:&why];
}
// 1.1.0: the desktop is landscape 960x540 by default, also in portrait (the
// owner's choice after testing Fit on an iPhone). A Fit size saved before this
// build is reset once; picking Fit again later is kept.
+ (void)migrateDesktopSizeToLandscape {
    NSUserDefaults *d=NSUserDefaults.standardUserDefaults;
    if([d boolForKey:@"LinuxRunnerDesktop.landscapeDefault.v1"])return;
    for(NSString *key in d.dictionaryRepresentation.allKeys)
        if([key hasPrefix:@"LinuxRunnerDesktop.resolution."]&&[d integerForKey:key]==4)[d setInteger:3 forKey:key];
    [d setBool:YES forKey:@"LinuxRunnerDesktop.landscapeDefault.v1"];
}
+ (void)prepareExecutionBackendAtLaunch {
    [self migrateDesktopSizeToLandscape];
    static dispatch_once_t observe;
    dispatch_once(&observe,^{[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){[LRRuntimeBridge retryJITWhenAuthorized];}];});
    NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
    NSString *crashedPath=[defaults stringForKey:LRJITAttemptKey];
    if(crashedPath){
        // The previous activation never returned: never retry that path automatically.
        NSArray *crashed=[defaults stringArrayForKey:LRJITCrashedPathsKey]?:@[];
        [defaults setObject:[crashed arrayByAddingObject:crashedPath] forKey:LRJITCrashedPathsKey];
        [defaults removeObjectForKey:LRJITAttemptKey];[defaults synchronize];
        LRJITDetail=[NSString stringWithFormat:@"Interpreter · the last JIT activation (%@) ended LinuxRunner; that method is disabled",crashedPath];
        [self logCategory:@"JIT" event:LRJITDetail];
    }
    NSString *mode=self.executionMode;
    if(!self.jitCompiledIn||[mode isEqual:@"interpreter"])return;
    // Automatic and JIT: use what iOS already allows (e.g. launched from StikDebug).
    if(!self.requiresPreparedJITRegion||self.debuggerAttached){NSString *why=nil;[self activateJIT:&why];}
    else LRJITDetail=@"Interpreter · attach with StikDebug in Settings › Execution Engine to start the JIT";
}
+ (BOOL)activeEnvironmentIsDebian {
    // Catalog identity is authoritative; no inference from a library name.
    NSDictionary *catalog=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:self.catalogURL]?:[NSData data] options:0 error:nil];
    for(NSDictionary *e in catalog[@"environments"])if([e[@"rootfsName"] isEqual:self.activeRootName])return [e[@"distributionID"] isEqual:@"debian"];
    NSDictionary *installation=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:[[self rootURL:self.activeRootName] URLByAppendingPathComponent:@"environment-install.json"]]?:[NSData data] options:0 error:nil];
    return [installation[@"distributionID"] isEqual:@"debian"];
}
+ (BOOL)assembleDebianPackages:(NSArray<NSString *> *)packages toArchive:(NSURL *)archive progress:(void (^)(double))progress cancel:(BOOL (^)(void))cancel error:(NSError **)error {
    LRInstallProgress *reporter=[LRInstallProgress new];reporter.progress=^(double value,NSString *message){progress(value);};reporter.cancel=cancel;
    const char **paths=calloc(packages.count,sizeof(char *));if(!paths){if(error)*error=lrError(ENOMEM,@"Package path allocation failed");return NO;}
    for(NSUInteger i=0;i<packages.count;i++)paths[i]=packages[i].fileSystemRepresentation;
    struct lr_debian_bootstrap_options options={.progress=LRPackageProgress,.cancel=LRPackageCancel,.log=LRPackageDiagnostic,.context=(__bridge void *)reporter,.trace_entries=atomic_load(&diagnosticLevel)>=3};
    char message[2048]={0};int result=lr_debian_bootstrap_diagnosed(paths,packages.count,archive.fileSystemRepresentation,&options,message,sizeof message);free(paths);
    [LRDiagnosticCenter flush];
    if(result&&error)*error=lrError(result,[NSString stringWithUTF8String:message]);return result==0;
}
+ (NSString *)readArchDatabase:(NSURL *)database error:(NSError **)error {
    char *text=NULL;size_t size=0;char message[1024]={0};
    int result=lr_arch_read_database(database.fileSystemRepresentation,&text,&size,message,sizeof message);
    if(result){if(error)*error=lrError(result,[NSString stringWithUTF8String:message]);return nil;}
    NSString *s=[[NSString alloc] initWithBytesNoCopy:text length:size encoding:NSUTF8StringEncoding freeWhenDone:YES];
    if(!s){free(text);if(error)*error=lrError(EILSEQ,@"Repository database is not UTF-8");}
    return s;
}
+ (BOOL)assembleArchPackages:(NSArray<NSString *> *)packages localNames:(NSArray<NSString *> *)names localDescriptions:(NSArray<NSString *> *)descriptions toArchive:(NSURL *)archive progress:(void (^)(double))progress cancel:(BOOL (^)(void))cancel error:(NSError **)error {
    if(names.count!=packages.count||descriptions.count!=packages.count){if(error)*error=lrError(EINVAL,@"Package metadata mismatch");return NO;}
    LRInstallProgress *reporter=[LRInstallProgress new];reporter.progress=^(double value,NSString *message){progress(value);};reporter.cancel=cancel;
    const char **paths=calloc(packages.count,sizeof(char *)),**nm=calloc(packages.count,sizeof(char *)),**ds=calloc(packages.count,sizeof(char *));
    if(!paths||!nm||!ds){free(paths);free(nm);free(ds);if(error)*error=lrError(ENOMEM,@"Package path allocation failed");return NO;}
    for(NSUInteger i=0;i<packages.count;i++){paths[i]=packages[i].fileSystemRepresentation;nm[i]=names[i].UTF8String;ds[i]=descriptions[i].UTF8String;}
    struct lr_debian_bootstrap_options options={.progress=LRPackageProgress,.cancel=LRPackageCancel,.log=LRPackageDiagnostic,.context=(__bridge void *)reporter,.trace_entries=atomic_load(&diagnosticLevel)>=3};
    char message[2048]={0};int result=lr_arch_bootstrap_diagnosed(paths,nm,ds,packages.count,archive.fileSystemRepresentation,&options,message,sizeof message);
    free(paths);free(nm);free(ds);[LRDiagnosticCenter flush];
    if(result&&error)*error=lrError(result,[NSString stringWithUTF8String:message]);return result==0;
}
+ (BOOL)importVerifiedEnvironment:(NSURL *)archive root:(NSString *)root sha256:(NSString *)sha256 metadata:(NSDictionary<NSString *,NSString *> *)metadata progress:(void (^)(double,NSString *))progress cancel:(BOOL (^)(void))cancel error:(NSError **)error {
    LRInstallProgress *reporter=[LRInstallProgress new];reporter.progress=progress;reporter.cancel=cancel;
    return [Roots.instance importVerifiedRootFromArchive:archive name:root sha256:sha256 metadata:metadata error:error progressReporter:reporter];
}
+ (BOOL)activeEnvironmentIsArch {
    NSDictionary *catalog=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:self.catalogURL]?:[NSData data] options:0 error:nil];
    for(NSDictionary *e in catalog[@"environments"])if([e[@"rootfsName"] isEqual:self.activeRootName])return [e[@"distributionID"] isEqual:@"arch"];
    NSDictionary *installation=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:[[self rootURL:self.activeRootName] URLByAppendingPathComponent:@"environment-install.json"]]?:[NSData data] options:0 error:nil];
    return [installation[@"distributionID"] isEqual:@"arch"];
}
+ (BOOL)importVerifiedEnvironment:(NSURL *)archive root:(NSString *)root sha256:(NSString *)sha256 progress:(void (^)(double,NSString *))progress cancel:(BOOL (^)(void))cancel error:(NSError **)error {
    LRInstallProgress *reporter=[LRInstallProgress new];reporter.progress=progress;reporter.cancel=cancel;
    return [Roots.instance importVerifiedRootFromArchive:archive name:root sha256:sha256 metadata:@{@"distributionID":@"debian",@"distribution":@"Debian GNU/Linux",@"version":@"13.7",@"packageManager":@"apt",@"defaultShell":@"/bin/bash"} error:error progressReporter:reporter];
}
+ (void)createEnvironment:(NSString *)root completion:(void (^)(NSError *))completion {
    if (!validRoot(root) || [Roots.instance.roots containsObject:root]) {
        completion(lrError(EINVAL, @"Invalid or duplicate environment identifier.")); return;
    }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSURL *archive = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"];
        BOOL imported = [Roots.instance importRootFromArchive:archive name:root error:&error progressReporter:nil];
        if (!imported && !error) error = lrError(EIO, @"The verified rootfs could not be imported.");
        [self logCategory:@"FS" event:error ? @"environment creation failed" : @"verified environment created"];
        dispatch_async(dispatch_get_main_queue(), ^{ completion(error); });
    });
}
+ (NSArray *)discoverApplications {
    // Retain a VFS context while holding the task locks; do not borrow a task
    // pointer across unlock or mutate another process's thread-local current.
    struct fs_info *fs = NULL;
    lock(&pids_lock);
    struct task *task = pid_get_task(1);
    if (task) {
        lock(&task->general_lock);
        fs = task->fs;
        if (fs) atomic_fetch_add(&fs->refcount, 1);
        unlock(&task->general_lock);
    }
    unlock(&pids_lock);
    if (!fs) return @[];
    // VFS permission checks require thread-local credentials. This private
    // read-only context has the guest's root credentials and retained fs only.
    struct task context = {.euid = 0, .egid = 0, .fs = fs};
    current = &context;
    NSMutableArray *result = [NSMutableArray new];
    for (NSString *name in @[@"python3", @"git", @"vim", @"nano", @"node", @"curl", @"htop", @"gcc", @"ffmpeg"]) {
        for (NSString *base in @[@"/usr/local/bin", @"/usr/bin", @"/bin"]) {
            NSString *path = [base stringByAppendingPathComponent:name];
            struct fd *fd = generic_open_in_fs(fs, path.UTF8String, O_RDONLY_, 0);
            if (IS_ERR(fd)) continue;
            struct statbuf st;
            BOOL executable = fd->mount->fs->fstat(fd, &st) == 0 && S_ISREG(st.mode) && (st.mode & 0111);
            fd_close(fd);
            if (executable) { [result addObject:@{@"name":name, @"command":path}]; break; }
        }
    }
    current = NULL;
    fs_info_release(fs);
    return result;
}
// Only inspect the current guest VFS. Host sandbox paths are never interpreted
// as desktop executables. Retain fs while reading, restore thread-local state.
static NSString *GuestExecutable(struct fs_info *fs, NSString *command) {
    NSArray *paths=[command hasPrefix:@"/"]?@[command]:@[[[@"/usr/local/bin/" stringByAppendingString:command] copy],[@"/usr/bin/" stringByAppendingString:command],[@"/bin/" stringByAppendingString:command]];
    for(NSString *path in paths){
        struct fd *fd=generic_open_in_fs(fs,path.UTF8String,O_RDONLY_,0);if(IS_ERR(fd))continue;
        struct statbuf st;BOOL valid=fd->mount->fs->fstat(fd,&st)==0&&S_ISREG(st.mode)&&(st.mode&0111);fd_close(fd);
        if(valid)return path;
    }return nil;
}
static NSData *GuestApplicationIcon(struct fs_info *fs, NSString *icon) {
    if (!icon.length || [icon containsString:@".."] || [icon containsString:@"\0"]) return nil;
    NSMutableArray *paths=[NSMutableArray new];
    if ([icon hasPrefix:@"/"]) [paths addObject:icon];
    else if (![icon containsString:@"/"]) {
        NSString *leaf=icon.pathExtension.length?icon:[icon stringByAppendingString:@".png"];
        for (NSString *base in @[@"/usr/share/icons/hicolor/64x64/apps", @"/usr/share/icons/hicolor/48x48/apps", @"/usr/share/pixmaps"])
            [paths addObject:[base stringByAppendingPathComponent:leaf]];
    }
    for (NSString *path in paths) {
        if (![@[@"png",@"jpg",@"jpeg"] containsObject:path.pathExtension.lowercaseString]) continue;
        struct fd *fd=generic_open_in_fs(fs,path.UTF8String,O_RDONLY_,0); if(IS_ERR(fd))continue;
        struct statbuf st; BOOL valid=fd->mount->fs->fstat(fd,&st)==0&&S_ISREG(st.mode)&&st.size>0&&st.size<=131072;
        NSMutableData *data=[NSMutableData new]; char buffer[4096]; ssize_t n;
        if(valid&&fd->ops->read) while(data.length<131072&&(n=fd->ops->read(fd,buffer,MIN(sizeof buffer,131072-data.length)))>0) [data appendBytes:buffer length:n];
        fd_close(fd); if(valid&&data.length==(NSUInteger)st.size)return data;
    }
    return nil;
}
+ (NSArray *)discoverGraphicalApplications {
    struct fs_info *fs=NULL;lock(&pids_lock);struct task *task=pid_get_task(1);
    if(task){lock(&task->general_lock);fs=task->fs;if(fs)atomic_fetch_add(&fs->refcount,1);unlock(&task->general_lock);}unlock(&pids_lock);
    if(!fs)return @[];
    struct task context={.euid=0,.egid=0,.fs=fs};struct task *previous=current;current=&context;
    NSMutableArray *result=[NSMutableArray new];NSMutableSet *seen=[NSMutableSet new];unsigned scanned=0;
    for(NSString *base in @[@"/root/.local/share/applications",@"/usr/local/share/applications",@"/usr/share/applications"]){
        struct fd *directory=generic_open_in_fs(fs,base.UTF8String,O_RDONLY_|O_DIRECTORY_,0);if(IS_ERR(directory))continue;
        struct dir_entry entry;
        while(scanned<256&&directory->ops->readdir&&directory->ops->readdir(directory,&entry)>0){
            NSString *leaf=[NSString stringWithUTF8String:entry.name];if(![leaf hasSuffix:@".desktop"]||[leaf containsString:@"/"])continue;scanned++;
            if([seen containsObject:leaf])continue;[seen addObject:leaf];
            NSString *path=[base stringByAppendingPathComponent:leaf];struct fd *file=generic_open_in_fs(fs,path.UTF8String,O_RDONLY_,0);if(IS_ERR(file))continue;
            struct statbuf st;BOOL valid=file->mount->fs->fstat(file,&st)==0&&S_ISREG(st.mode)&&st.size<=65536;
            NSMutableData *data=[NSMutableData new];char buffer[4096];ssize_t n;
            if(valid&&file->ops->read){while(data.length<65536&&(n=file->ops->read(file,buffer,MIN(sizeof buffer,65536-data.length)))>0)[data appendBytes:buffer length:n];}
            fd_close(file);NSString *text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];if(!text)continue;
            NSDictionary *parsed=[LRDesktopEntry configuration:text path:path];if(!parsed)continue;
            NSString *executable=GuestExecutable(fs,parsed[@"executable"]);if(!executable)continue;
            NSString *tryExec=parsed[@"tryExec"];if(tryExec.length&&!GuestExecutable(fs,tryExec))continue;
            NSMutableDictionary *config=[parsed mutableCopy];config[@"executable"]=executable;config[@"root"]=self.activeRootName;
            NSData *icon=GuestApplicationIcon(fs,parsed[@"icon"]); if(icon)config[@"iconPNG"]=icon;
            [result addObject:config];
        }fd_close(directory);
    }
    current=previous;fs_info_release(fs);
    return result;
}
// Retain PID 1's fs rather than reading fakefs host backing paths. The private
// inspection context is restored on every exit and never becomes a live task.
static struct fs_info *LRAccountFS(void) {
    struct fs_info *fs = NULL;
    lock(&pids_lock);
    struct task *task = pid_get_task(1);
    if (task) {
        lock(&task->general_lock);
        fs = task->fs;
        if (fs) atomic_fetch_add(&fs->refcount, 1);
        unlock(&task->general_lock);
    }
    unlock(&pids_lock);
    return fs;
}
+ (NSArray *)linuxAccounts {
    struct fs_info *fs = LRAccountFS();
    if (!fs) return @[];
    struct task context = {.euid = 0, .egid = 0, .fs = fs};
    struct task *previous = current; current = &context;
    struct fd *file = generic_open_in_fs(fs, "/etc/passwd", O_RDONLY_, 0);
    NSMutableData *data = [NSMutableData new];
    BOOL valid = NO;
    if (!IS_ERR(file)) {
        struct statbuf st;
        valid = file->mount->fs->fstat(file, &st) == 0 && S_ISREG(st.mode) && st.size <= 262144;
        char buffer[4096]; ssize_t count;
        if (valid && file->ops->read) {
            while (data.length < 262144 && (count = file->ops->read(file, buffer, MIN(sizeof buffer, 262144-data.length))) > 0)
                [data appendBytes:buffer length:count];
            valid = data.length == (NSUInteger)st.size;
        } else valid = NO;
        fd_close(file);
    }
    current = previous; fs_info_release(fs);
    NSString *text = valid ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
    NSMutableArray *accounts = [NSMutableArray new];
    NSMutableSet *seen = [NSMutableSet new];
    for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        NSArray<NSString *> *fields = [line componentsSeparatedByString:@":"];
        if (fields.count != 7 || !lr_linux_account_name_valid(fields[0].UTF8String) || [seen containsObject:fields[0]]) continue;
        NSCharacterSet *nonDigits = [NSCharacterSet characterSetWithCharactersInString:@"0123456789"].invertedSet;
        if (!fields[2].length || !fields[3].length || [fields[2] rangeOfCharacterFromSet:nonDigits].location != NSNotFound ||
            [fields[3] rangeOfCharacterFromSet:nonDigits].location != NSNotFound || fields[2].longLongValue >= UINT32_MAX || fields[3].longLongValue >= UINT32_MAX) continue;
        // Never expose aliases of UID 0 as ordinary selectable users.
        if ((fields[2].longLongValue == 0 && ![fields[0] isEqual:@"root"]) ||
            ![fields[5] hasPrefix:@"/"] || ![fields[6] hasPrefix:@"/"] ||
            [@[@"nologin", @"false"] containsObject:fields[6].lastPathComponent]) continue;
        [seen addObject:fields[0]];
        [accounts addObject:@{@"name":fields[0], @"uid":fields[2], @"gid":fields[3], @"home":fields[5], @"shell":fields[6]}];
        if (accounts.count >= 1024) break;
    }
    return accounts;
}
static NSString *LRDefaultAccountKey(void) {
    return [@"LinuxRunnerDefaultUser." stringByAppendingString:LRRuntimeBridge.activeRootName];
}
+ (NSString *)defaultLinuxUser {
    return [NSUserDefaults.standardUserDefaults stringForKey:LRDefaultAccountKey()] ?: @"root";
}
+ (BOOL)setDefaultLinuxUser:(NSString *)name error:(NSError **)error {
    for (NSDictionary *account in self.linuxAccounts) {
        if ([account[@"name"] isEqual:name]) {
            [NSUserDefaults.standardUserDefaults setObject:name forKey:LRDefaultAccountKey()];
            [self applySharedFolderOwnership];
            return YES;
        }
    }
    if (error) *error = lrError(ENOENT, @"The account is not a login account in the active guest /etc/passwd. Refresh the list.");
    return NO;
}
static NSString *LRAccountTool(NSArray<NSString *> *paths) {
    struct fs_info *fs = LRAccountFS(); if (!fs) return nil;
    struct task context = {.euid=0, .egid=0, .fs=fs}; struct task *previous = current; current=&context;
    NSString *tool = nil;
    for (NSString *path in paths) if ((tool = GuestExecutable(fs, path))) break;
    current=previous; fs_info_release(fs); return tool;
}
static NSDictionary *LRLoginAccount(NSString *name) {
    if (!lr_linux_account_name_valid(name.UTF8String)) return nil;
    for (NSDictionary *account in LRRuntimeBridge.linuxAccounts) if ([account[@"name"] isEqual:name]) return account;
    return nil;
}
+ (NSArray<NSDictionary<NSString *, NSString *> *> *)sharedFolders { return iosfs_external_folder_mounts(); }
+ (BOOL)addSharedFolder:(NSURL *)url error:(NSError **)error {
    if (!self.isRunning) { if (error) *error = lrError(EAGAIN, @"Start the Linux runtime first."); return NO; }
    if (!iosfs_add_external_folder_mount_url(url, error)) return NO;
    [self applySharedFolderOwnership];
    return YES;
}
+ (void)removeSharedFolder:(NSString *)name { iosfs_remove_external_folder_mount(name); }
// Shared folder check: iOS side (security-scoped URL, item count) and Linux
// side, as the default Linux user (mount point, item count, write/read/delete
// probe). A difference shows where the bridge breaks; logged as FILES.
static int LRDatalessPolicyResult = -1;
void LRSetDatalessMaterialization(void) {
    LRDatalessPolicyResult = setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_PROCESS, IOPOL_MATERIALIZE_DATALESS_FILES_ON) == 0 ? 1 : 0;
    LRBootEvent(LRDatalessPolicyResult ? "[FILES] dataless materialization on" : "[FILES] dataless materialization unavailable");
}
+ (void)checkSharedFolder:(NSString *)name completion:(void (^)(NSString *summary))completion {
    NSDictionary *folder = nil; for (NSDictionary *f in iosfs_external_folder_mounts()) if ([f[@"name"] isEqual:name]) folder = f;
    if (!folder) { completion(@"This shared folder is no longer registered."); return; }
    NSURL *url = iosfs_external_folder_url(name); NSError *listError = nil;
    NSMutableArray *iosLines = [NSMutableArray new];
    if (!url) [iosLines addObject:@"iOS: access not restored (bookmark could not be resolved or permission was revoked; the folder may be moved or its drive disconnected)"];
    else {
        NSArray *items = [NSFileManager.defaultManager contentsOfDirectoryAtURL:url includingPropertiesForKeys:@[NSURLUbiquitousItemDownloadingStatusKey] options:0 error:&listError];
        NSUInteger dataless = 0; for (NSURL *item in items) { NSString *status = nil; [item getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil]; if (status && ![status isEqual:NSURLUbiquitousItemDownloadingStatusCurrent]) dataless++; }
        NSNumber *ubiquitous = nil, *local = nil; [url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil]; [url getResourceValue:&local forKey:NSURLVolumeIsLocalKey error:nil];
        NSString *volume = nil; [url getResourceValue:&volume forKey:NSURLVolumeNameKey error:nil];
        NSString *kind = ubiquitous.boolValue ? @"iCloud Drive" : [url.path containsString:@"/LiveFiles/"] ? @"external drive" : [url.path containsString:@"File Provider Storage"] ? @"On My iPhone / app folder" : @"other provider";
        [iosLines addObject:items ? [NSString stringWithFormat:@"iOS: %lu items (%lu not downloaded; download-on-open %@) · %@ · volume %@", (unsigned long)items.count, (unsigned long)dataless, LRDatalessPolicyResult == 1 ? @"on" : @"off", kind, volume ?: @"?"]
                                  : [NSString stringWithFormat:@"iOS: cannot list — %@ · %@", listError.localizedDescription, kind]];
        // The Linux mount point is a link inside the environment's filesystem to this folder.
        NSURL *link = [[[self rootURL:self.activeRootName] URLByAppendingPathComponent:@"data"] URLByAppendingPathComponent:[folder[@"mountPoint"] substringFromIndex:1]];
        struct stat linkTarget, host; char target[PATH_MAX]; ssize_t n = readlink(link.fileSystemRepresentation, target, sizeof target - 1);
        BOOL same = stat(link.fileSystemRepresentation, &linkTarget) == 0 && stat(url.fileSystemRepresentation, &host) == 0 && linkTarget.st_dev == host.st_dev && linkTarget.st_ino == host.st_ino;
        [iosLines addObject:[NSString stringWithFormat:@"Mount: %@ link=%@ resolves-to-folder=%@", folder[@"mountPoint"], n > 0 ? @"present" : [NSString stringWithFormat:@"missing (errno %d)", errno], same ? @"yes" : @"NO"]];
    }
    NSString *ios = [iosLines componentsJoinedByString:@"\n"];
    NSError *error = nil; NSURL *support = [LRGUIViewController prepareSupport:&error];
    if (!support || !self.isRunning) { completion([ios stringByAppendingString:@"\nLinux: runtime not running"]); return; }
    NSString *token = [[NSUUID.UUID.UUIDString substringToIndex:8] lowercaseString];
    NSString *resultName = [NSString stringWithFormat:@"share-check-%@.txt", name];
    NSURL *result = [support URLByAppendingPathComponent:resultName]; [NSFileManager.defaultManager removeItemAtURL:result error:nil];
    NSString *guestOut = [@"/home/mobile/Downloads/.linuxrunner/gui/" stringByAppendingString:resultName];
    // Root runs the check as the default user (writing to /tmp), then publishes
    // the result where the app reads it (the support folder is root-only).
    NSString *script = @"u=$1 d=$2 out=$3 token=$4 tmp=/tmp/lr-share-check.$$\n"
        "if [ \"$u\" = root ]; then sh /home/mobile/Downloads/.linuxrunner/gui/share-check.sh \"$d\" \"$tmp\" \"$token\"\n"
        "else su -l \"$u\" -s /bin/sh -c \"sh /home/mobile/Downloads/.linuxrunner/gui/share-check.sh '$d' '$tmp' '$token'\"; fi\n"
        "cp \"$tmp\" \"$out.tmp\" && mv \"$out.tmp\" \"$out\"; rm -f \"$tmp\"\n";
    NSArray *command = @[@"/bin/sh", @"-c", script, @"linuxrunner-share-check", self.defaultLinuxUser, folder[@"mountPoint"], guestOut, token];
    TerminalViewController *process = [self terminalForCommand:command directory:@"/"];
    LRWaitForProcess(process, 60, ^(BOOL ended) {
        (void)process;
        NSString *text = [[NSString stringWithContentsOfURL:result encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSString *linux = text.length ? [@"Linux: " stringByAppendingString:[text stringByReplacingOccurrencesOfString:@"\n" withString:@" · "]] : @"Linux: the check did not finish";
        // Round trip: the file Linux created and renamed must be visible to iOS.
        NSString *roundTrip = @"";
        if (url && [text containsString:@"rename=ok"]) {
            NSURL *kept = [url URLByAppendingPathComponent:[NSString stringWithFormat:@"LinuxRunner-check-%@-renamed.txt", token]];
            NSString *content = [NSString stringWithContentsOfURL:kept encoding:NSUTF8StringEncoding error:nil];
            BOOL seen = [content hasPrefix:@"linuxrunner-check\nmodified"];
            BOOL removed = [NSFileManager.defaultManager removeItemAtURL:kept error:nil];
            roundTrip = [NSString stringWithFormat:@"\nRound trip: Linux file visible in iOS=%@ removed-by-iOS=%@", seen ? @"yes" : @"NO", removed ? @"yes" : @"no"];
        }
        [self logCategory:@"FILES" event:[NSString stringWithFormat:@"shared folder check %@ | %@ | %@%@", folder[@"mountPoint"], [ios stringByReplacingOccurrencesOfString:@"\n" withString:@" | "], linux, [roundTrip stringByReplacingOccurrencesOfString:@"\n" withString:@" "]]];
        completion([NSString stringWithFormat:@"%@\n%@%@", ios, linux, roundTrip]);
    });
}
// Reuse the existing bind bridge; install a per-root app-controlled Files folder.
+ (void)prepareUserSharedFolder:(NSDictionary *)account {
    if(!self.isRunning)return;
    int stage=LRTransferStaging();if(stage>=0)close(stage);
    NSURL *shared=[self sharedExchangeURL:self.activeRootName];
    if(![NSFileManager.defaultManager createDirectoryAtURL:shared withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil])return;
    struct fs_info *fs=LRAccountFS();if(!fs)return;
    uint32_t uid=[account[@"uid"] unsignedIntValue],gid=[account[@"gid"] unsignedIntValue];
    struct task context={.uid=0,.euid=0,.gid=0,.egid=0,.fs=fs};struct task *previous=current;current=&context;
    int result=generic_mkdirat(AT_PWD,"/mnt",0755);
    if(!result||result==_EEXIST)result=fakefs_bind_mount("/mnt/LinuxRunnerShared",shared.fileSystemRepresentation,false);
    if(!result)result=fakefs_bind_mount_set_owner("/mnt/LinuxRunnerShared",uid,gid);
    // Preserve any existing Shared file/directory/link. Never replace user data.
    NSString *home=account[@"home"];
    if(!result&&home.length){NSString *alias=[home stringByAppendingPathComponent:@"Shared"];struct statbuf st;
        context.uid=context.euid=uid;context.gid=context.egid=gid;
        if(generic_statat(AT_PWD,alias.UTF8String,&st,false)==_ENOENT)generic_symlinkat("/mnt/LinuxRunnerShared",AT_PWD,alias.UTF8String);
    }
    current=previous;fs_info_release(fs);
    [self logCategory:@"FILES" event:[NSString stringWithFormat:@"persistent shared folder mount result=%d",result]];
}
+ (void)applySharedFolderOwnership {
    NSDictionary *account = LRLoginAccount(self.defaultLinuxUser);
    uint32_t uid = [account[@"uid"] unsignedIntValue], gid = [account[@"gid"] unsignedIntValue];
    // Downloads was root-owned despite selecting a normal user: imports were
    // readable but that user could not create/edit files for export.
    fakefs_bind_mount_set_owner("/home/mobile/Downloads",uid,gid);
    [self prepareUserSharedFolder:account];
    for (NSDictionary *folder in iosfs_external_folder_mounts()) {
        NSString *mountPoint = folder[@"mountPoint"];
        int err = fakefs_bind_mount_set_owner(mountPoint.fileSystemRepresentation, uid, gid);
        [self logCategory:@"FILES" event:[NSString stringWithFormat:@"shared folder %@ owner=%u:%u result=%d", folder[@"mountPoint"], uid, gid, err]];
    }
}
+ (BOOL)publishDesktopRequest:(NSString *)path user:(NSString *)user error:(NSError **)error {
    NSDictionary *account=LRLoginAccount(user); struct fs_info *fs=LRAccountFS();
    if (!account || !fs) { if (fs) fs_info_release(fs); if(error)*error=lrError(EINVAL,@"The desktop account is no longer available."); return NO; }
    struct task context={.uid=[account[@"uid"] unsignedIntValue], .euid=[account[@"uid"] unsignedIntValue],
        .gid=[account[@"gid"] unsignedIntValue], .egid=[account[@"gid"] unsignedIntValue], .fs=fs};
    struct task *previous=current; current=&context;
    int result=generic_renameat(AT_PWD, [path stringByAppendingString:@".pending"].UTF8String, AT_PWD, path.UTF8String);
    current=previous; fs_info_release(fs);
    if(result && error)*error=lrError(-result,@"Cannot publish the desktop launch request.");
    return result == 0;
}
+ (BOOL)writeDesktopRequest:(NSData *)data guestPath:(NSString *)path user:(NSString *)user error:(NSError **)error {
    NSDictionary *account = LRLoginAccount(user);
    struct fs_info *fs = LRAccountFS();
    if (!account || !fs || data.length > 8192) {
        if (fs) fs_info_release(fs);
        if (error) *error = lrError(EINVAL, @"Desktop identity or launch request is invalid.");
        return NO;
    }
    struct task context = {.uid=[account[@"uid"] unsignedIntValue], .euid=[account[@"uid"] unsignedIntValue],
        .gid=[account[@"gid"] unsignedIntValue], .egid=[account[@"gid"] unsignedIntValue], .fs=fs};
    struct task *previous = current; current = &context;
    struct fd *file = generic_open_in_fs(fs, path.UTF8String, O_WRONLY_ | O_CREAT_ | O_EXCL_ | O_NOFOLLOW_, 0600);
    int result = IS_ERR(file) ? PTR_ERR(file) : 0;
    if (!result) {
        size_t offset = 0;
        while (offset < data.length) {
            ssize_t count = file->ops->write ? file->ops->write(file, (const char *)data.bytes + offset, data.length-offset) : _EBADF;
            if (count <= 0) { result = count < 0 ? (int)count : _EIO; break; }
            offset += count;
        }
        fd_close(file);
    }
    current=previous; fs_info_release(fs);
    if (result && error) *error=lrError(-result, @"Cannot publish the desktop request with the selected guest permissions.");
    return result == 0;
}
static NSArray<NSString *> *LRLoginCommand(NSString *name, NSError **error) {
    if (!LRLoginAccount(name)) {
        if (error) *error = lrError(ENOENT, @"The selected Linux account no longer exists or does not have a login shell.");
        return nil;
    }
    NSString *su = LRAccountTool(@[@"/bin/su", @"/usr/bin/su"]);
    if (!su) { if (error) *error = lrError(ENOENT, @"Install the distribution's su/login tools first."); return nil; }
    // Guest su implements UID/GID, supplementary groups, HOME, USER, LOGNAME,
    // login directory and shell. Root parent is retained for administration.
    return @[su, @"-l", name];
}
+ (NSArray *)defaultLinuxLoginCommand:(NSError **)error {
    NSString *name = self.defaultLinuxUser;
    if ([name isEqual:@"root"]) return nil;
    return LRLoginCommand(name, error);
}
+ (TerminalViewController *)terminalForLinuxUser:(NSString *)name error:(NSError **)error {
    NSArray *command = LRLoginCommand(name, error);
    return command ? [self terminalForCommand:command directory:@"/"] : nil;
}
+ (TerminalViewController *)createLinuxUser:(NSString *)name error:(NSError **)error { return [self createLinuxUser:name administrator:NO error:error]; }
+ (BOOL)canGrantAdministrator { return LRAccountTool(@[@"/usr/bin/sudo", @"/bin/sudo"]) != nil; }
+ (TerminalViewController *)createLinuxUser:(NSString *)name administrator:(BOOL)administrator error:(NSError **)error {
    if (!lr_linux_account_name_valid(name.UTF8String) || [name isEqual:@"root"]) {
        if (error) *error=lrError(EINVAL, @"Use 1–32 lowercase ASCII letters, digits, underscores or hyphens, starting with a letter or underscore."); return nil;
    }
    // Use getent semantics inside the real account tool too: its own database
    // locking and duplicate-name checks are authoritative, not this UI list.
    NSString *passwd=LRAccountTool(@[@"/usr/bin/passwd", @"/bin/passwd"]);
    // shadow's useradd on glibc distributions (Debian, Arch Linux ARM); BusyBox adduser on Alpine.
    NSString *create=LRAccountTool(self.activeEnvironmentIsDebian || self.activeEnvironmentIsArch ? @[@"/usr/sbin/useradd", @"/usr/bin/useradd"] : @[@"/usr/sbin/adduser", @"/sbin/adduser", @"/usr/bin/adduser", @"/bin/adduser"]);
    if (!passwd || !create) { if(error)*error=lrError(ENOENT, @"Account tools are not installed yet. Use Create Account in Linux Users; it installs them automatically."); return nil; }
    // Administrator: the distribution's sudo group (sudo on Debian, wheel on
    // Alpine); sudo then asks for the user's own password.
    NSString *grant=@"";
    if (administrator) {
        if (!self.canGrantAdministrator) { if(error)*error=lrError(ENOENT, @"sudo is not installed in this environment; install it first, or create the user without administrator access."); return nil; }
        grant=self.activeEnvironmentIsDebian ? @" && { getent group sudo >/dev/null || groupadd --system sudo; } && usermod -aG sudo -- \"$1\"" :
            self.activeEnvironmentIsArch ? @" && { getent group wheel >/dev/null || groupadd --system wheel; } && usermod -aG wheel -- \"$1\""
             " && { [ -e /etc/sudoers.d/linuxrunner-wheel ] || { mkdir -p /etc/sudoers.d && printf '%%wheel ALL=(ALL:ALL) ALL\\n' > /etc/sudoers.d/linuxrunner-wheel && chmod 0440 /etc/sudoers.d/linuxrunner-wheel; }; }" :
            @" && { getent group wheel >/dev/null || addgroup -S wheel; } && addgroup \"$1\" wheel"
             " && { [ -e /etc/sudoers.d/linuxrunner-wheel ] || { mkdir -p /etc/sudoers.d && printf '%%wheel ALL=(ALL:ALL) ALL\\n' > /etc/sudoers.d/linuxrunner-wheel && chmod 0440 /etc/sudoers.d/linuxrunner-wheel; }; }";
    }
    NSString *script=[NSString stringWithFormat:self.activeEnvironmentIsDebian || self.activeEnvironmentIsArch ?
        @"\"$2\" --create-home --shell /bin/bash -- \"$1\"%@ && exec \"$3\" \"$1\"" :
        @"\"$2\" -D -h \"/home/$1\" -s /bin/sh \"$1\"%@ && exec \"$3\" \"$1\"", grant];
    return [self terminalForCommand:@[@"/bin/sh", @"-c", script, @"linuxrunner-create-user", name, create, passwd] directory:@"/"];
}
// ---- Native account setup ---------------------------------------------------
// The app hashes the password (SHA-512 crypt) and the hash goes to chpasswd -e
// through the process's terminal input: never argv, environment or a file.
// Missing account tools (LinuxRunner's Debian image has no passwd package) and
// sudo for administrators are installed first by desktop-profile.sh.
static NSData *LRReadGuestFile(NSString *path, size_t limit) {
    struct fs_info *fs = LRAccountFS(); if (!fs) return nil;
    struct task context = {.euid = 0, .egid = 0, .fs = fs}; struct task *previous = current; current = &context;
    NSMutableData *data = nil;
    struct fd *file = generic_open_in_fs(fs, path.UTF8String, O_RDONLY_, 0);
    if (!IS_ERR(file)) {
        data = [NSMutableData new]; char buffer[4096]; ssize_t count;
        while (file->ops->read && data.length < limit && (count = file->ops->read(file, buffer, MIN(sizeof buffer, limit - data.length))) > 0) [data appendBytes:buffer length:count];
        fd_close(file);
    }
    current = previous; fs_info_release(fs);
    return data;
}
+ (BOOL)accountToolsPresent {
    return LRAccountTool(@[@"/usr/bin/passwd", @"/bin/passwd"]) && LRAccountTool(@[@"/usr/sbin/chpasswd", @"/sbin/chpasswd", @"/usr/bin/chpasswd", @"/bin/chpasswd"]) &&
        LRAccountTool(@[@"/usr/sbin/useradd", @"/usr/sbin/adduser", @"/sbin/adduser", @"/usr/bin/adduser", @"/bin/adduser"]);
}
static void LRWaitForProcess(TerminalViewController *process, NSTimeInterval timeout, void (^done)(BOOL ended)) {
    NSDate *start = NSDate.date;
    [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:YES block:^(NSTimer *timer) {
        BOOL ended = process.sessionPid <= 0;
        if (ended || -[start timeIntervalSinceNow] > timeout) { [timer invalidate]; done(ended); }
    }];
}
+ (void)createLinuxUser:(NSString *)name password:(NSString *)password administrator:(BOOL)administrator
               progress:(void (^)(NSString *))progress completion:(void (^)(NSError *))completion {
    if (!lr_linux_account_name_valid(name.UTF8String) || [name isEqual:@"root"]) { completion(lrError(EINVAL, @"Use 1–32 lowercase letters, digits, - or _, starting with a letter.")); return; }
    if (password.length < 4) { completion(lrError(EINVAL, @"Use a password of at least 4 characters.")); return; }
    for (NSDictionary *account in self.linuxAccounts) if ([account[@"name"] isEqual:name]) { completion(lrError(EEXIST, @"That Linux account already exists; choose it under Existing Accounts.")); return; }
    char salt[17], hash[160];
    if (lr_random_salt(salt) || lr_sha512_crypt(password.UTF8String, salt, hash, sizeof hash)) { completion(lrError(EIO, @"The password could not be prepared.")); return; }
    NSString *hashed = @(hash); memset(hash, 0, sizeof hash);
    BOOL debian = self.activeEnvironmentIsDebian, arch = self.activeEnvironmentIsArch;
    void (^create)(void) = ^{
        progress(@"Creating your account");
        NSString *grant = !administrator ? @"" : debian ?
            @"{ getent group sudo >/dev/null || groupadd --system sudo; }; usermod -aG sudo -- \"$1\"\n" : arch ?
            @"{ getent group wheel >/dev/null || groupadd --system wheel; }; usermod -aG wheel -- \"$1\"\n"
             "[ -e /etc/sudoers.d/linuxrunner-wheel ] || { mkdir -p /etc/sudoers.d && printf '%%wheel ALL=(ALL:ALL) ALL\\n' > /etc/sudoers.d/linuxrunner-wheel && chmod 0440 /etc/sudoers.d/linuxrunner-wheel; }\n" :
            @"{ getent group wheel >/dev/null || addgroup -S wheel; }; addgroup \"$1\" wheel\n"
             "[ -e /etc/sudoers.d/linuxrunner-wheel ] || { mkdir -p /etc/sudoers.d && printf '%%wheel ALL=(ALL:ALL) ALL\\n' > /etc/sudoers.d/linuxrunner-wheel && chmod 0440 /etc/sudoers.d/linuxrunner-wheel; }\n";
        NSString *script = [NSString stringWithFormat:@"set -e\nstty -echo 2>/dev/null || true\n"
            "if command -v useradd >/dev/null; then useradd --create-home --shell /bin/bash -- \"$1\"; else adduser -D -h \"/home/$1\" -s /bin/sh \"$1\"; fi\n"
            "%@IFS= read -r hash\nprintf '%%s:%%s\\n' \"$1\" \"$hash\" | chpasswd -e\n", grant];
        TerminalViewController *process = [self terminalForCommand:@[@"/bin/sh", @"-c", script, @"linuxrunner-create-user", name] directory:@"/"];
        [process.sessionTerminal sendInput:[[hashed stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
        LRWaitForProcess(process, 120, ^(BOOL ended) {
            (void)process;
            NSString *shadow = [[NSString alloc] initWithData:LRReadGuestFile(@"/etc/shadow", 1 << 20) ?: [NSData data] encoding:NSUTF8StringEncoding];
            BOOL stored = NO;
            for (NSString *line in [shadow componentsSeparatedByString:@"\n"]) {
                NSArray *fields = [line componentsSeparatedByString:@":"];
                if (fields.count > 1 && [fields[0] isEqual:name] && [fields[1] isEqual:hashed]) stored = YES;
            }
            BOOL exists = NO; for (NSDictionary *account in self.linuxAccounts) if ([account[@"name"] isEqual:name]) exists = YES;
            [self logCategory:@"ACCOUNT" event:[NSString stringWithFormat:@"native create user=%@ admin=%d ended=%d exists=%d password_set=%d", name, administrator, ended, exists, stored]];
            if (exists && stored) completion(nil);
            else completion(lrError(EIO, exists ? @"The account was created but its password could not be set. Set it in Linux Users." : @"The account could not be created. Details are in Runtime Diagnostics (ACCOUNT)."));
        });
    };
    [self ensureAccountTools:administrator progress:progress then:create failure:completion];
}
// Installs missing account tools (and sudo) through desktop-profile.sh accounts.
+ (void)ensureAccountTools:(BOOL)administrator progress:(void (^)(NSString *))progress then:(void (^)(void))then failure:(void (^)(NSError *))failure {
    BOOL needTools = !self.accountToolsPresent, needSudo = administrator && !self.canGrantAdministrator;
    if (!needTools && !needSudo) { then(); return; }
    NSArray *arguments = administrator ? @[@"accounts", @"sudo"] : @[@"accounts"];
    [self runSupportInstall:arguments title:needTools ? @"Installing Account Tools" : @"Installing Administrator Tools (sudo)" progress:progress completion:^(NSError *error) {
        // The installer verified the tools by running them; check the files the
        // account steps use as well before reporting success.
        BOOL ok = !error && self.accountToolsPresent && (!administrator || self.canGrantAdministrator);
        [self logCategory:@"ACCOUNT" event:[NSString stringWithFormat:@"account tools install ok=%d error=%@", ok, error.localizedDescription ?: @"none"]];
        if (ok) then();
        else failure(error ?: lrError(EIO, @"The account tools were reported installed but are missing. Export the Open Desktop Report and try again."));
    }];
}
+ (void)setLinuxPassword:(NSString *)name password:(NSString *)password progress:(void (^)(NSString *))progress completion:(void (^)(NSError *))completion {
    if (password.length < 4) { completion(lrError(EINVAL, @"Use a password of at least 4 characters.")); return; }
    BOOL exists = NO; for (NSDictionary *account in self.linuxAccounts) if ([account[@"name"] isEqual:name]) exists = YES;
    if (!exists) { completion(lrError(ENOENT, @"That Linux account no longer exists.")); return; }
    char salt[17], hash[160];
    if (lr_random_salt(salt) || lr_sha512_crypt(password.UTF8String, salt, hash, sizeof hash)) { completion(lrError(EIO, @"The password could not be prepared.")); return; }
    NSString *hashed = @(hash); memset(hash, 0, sizeof hash);
    [self ensureAccountTools:NO progress:progress then:^{
        progress(@"Setting the password");
        TerminalViewController *process = [self terminalForCommand:@[@"/bin/sh", @"-c", @"stty -echo 2>/dev/null || true\nIFS= read -r hash\nprintf '%s:%s\\n' \"$1\" \"$hash\" | chpasswd -e\n", @"linuxrunner-set-password", name] directory:@"/"];
        [process.sessionTerminal sendInput:[[hashed stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
        LRWaitForProcess(process, 60, ^(BOOL ended) {
            (void)process;
            NSString *shadow = [[NSString alloc] initWithData:LRReadGuestFile(@"/etc/shadow", 1 << 20) ?: [NSData data] encoding:NSUTF8StringEncoding];
            BOOL stored = NO;
            for (NSString *line in [shadow componentsSeparatedByString:@"\n"]) { NSArray *f = [line componentsSeparatedByString:@":"]; if (f.count > 1 && [f[0] isEqual:name] && [f[1] isEqual:hashed]) stored = YES; }
            [self logCategory:@"ACCOUNT" event:[NSString stringWithFormat:@"native password user=%@ ended=%d set=%d", name, ended, stored]];
            completion(stored ? nil : lrError(EIO, @"The password could not be set."));
        });
    } failure:completion];
}
// Desktop components for the active environment (setup wizard): the same
// targeted repair as Open Desktop, reporting desktop-profile.sh's stages.
+ (void)prepareDesktopProfile:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"repair" progress:progress completion:completion];
}
+ (void)installPlasma:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"plasma" progress:progress completion:completion];
}
+ (BOOL)plasmaInstalled {
    for (NSString *path in @[@"/usr/bin/startplasma-x11", @"/usr/bin/kwin_x11", @"/usr/bin/plasmashell"]) if (!LRReadGuestFile(path, 4).length) return NO;
    return YES;
}
+ (void)installLXQt:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"lxqt" progress:progress completion:completion];
}
+ (BOOL)lxqtInstalled {
    for (NSString *path in @[@"/usr/bin/startlxqt", @"/usr/bin/lxqt-panel", @"/usr/bin/pcmanfm-qt"]) if (!LRReadGuestFile(path, 4).length) return NO;
    return YES;
}
+ (void)installEnlightenment:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"enlightenment" progress:progress completion:completion];
}
+ (BOOL)enlightenmentInstalled {
    for (NSString *path in @[@"/usr/bin/enlightenment_start", @"/usr/bin/terminology"]) if (!LRReadGuestFile(path, 4).length) return NO;
    return YES;
}
+ (void)installWayfire:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"wayfire" progress:progress completion:completion];
}
+ (BOOL)wayfireInstalled {
    for (NSString *path in @[@"/usr/bin/wayfire", @"/usr/bin/wf-panel", @"/usr/bin/foot"]) if (!LRReadGuestFile(path, 4).length) return NO;
    return YES;
}
+ (void)installHyprland:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    [self runDesktopProfile:@"hyprland" progress:progress completion:completion];
}
+ (BOOL)hyprlandInstalled {
    for (NSString *path in @[@"/usr/bin/Hyprland", @"/usr/bin/waybar", @"/usr/bin/fuzzel", @"/usr/bin/seatd"]) if (!LRReadGuestFile(path, 4).length) return NO;
    return YES;
}
+ (void)runDesktopProfile:(NSString *)mode progress:(void (^)(NSString *stage))progress completion:(void (^)(NSError * _Nullable error))completion {
    NSString *title = [mode isEqual:@"plasma"] ? @"Installing KDE Plasma" : [mode isEqual:@"lxqt"] ? @"Installing LXQt" : [mode isEqual:@"enlightenment"] ? @"Installing Enlightenment" : [mode isEqual:@"wayfire"] ? @"Installing Wayfire" : [mode isEqual:@"hyprland"] ? @"Installing Hyprland" : @"Installing Desktop Components";
    [self runSupportInstall:@[mode] title:title progress:progress completion:completion];
}

// ---- Support installations: one runner, real progress -------------------------
// installer-run.sh desktop-profile.sh <mode>: the log carries PROFILE_STAGE /
// PACKAGE_STAGE lines, PACKAGE_PROGRESS <done> <total> while Debian's first
// start configures packages, and PROFILE_FAILURE category=…; apt writes its own
// percentages (APT::Status-Fd) to apt-status. Nothing here invents a number.
NSNotificationName const LRInstallProgressNotification = @"LRInstallProgressNotification";
static NSString *LRActiveInstall;
+ (BOOL)installationRunning { return LRActiveInstall != nil; }
static NSString *LRInstallFailureMessage(NSString *category, NSString *detail) {
    NSDictionary *advice = @{
        @"NETWORK_UNAVAILABLE": @"LinuxRunner could not reach the package servers. Check the internet connection and try again.",
        @"TLS_CERTIFICATE_ERROR": @"A secure connection to the package servers failed. Check that the date and time are correct, then try again.",
        @"PACKAGE_MANAGER_BUSY": @"Another package operation is using the package manager. Wait for it to finish, then try again.",
        @"INSUFFICIENT_STORAGE": @"There is not enough free storage on this iPhone. Free some space and try again.",
        @"PACKAGE_NOT_FOUND": @"The package servers did not offer a required package. Try again later.",
        @"REPOSITORY_METADATA_ERROR": @"The package lists could not be verified or downloaded. Try again later.",
        @"DEPENDENCY_CONFLICT": @"The packages have conflicting dependencies. Export the Open Desktop Report and send it.",
        @"PACKAGE_DATABASE_RECOVERY_REQUIRED": @"Linux package setup needs attention. Review the retained package log and Repair Center before retrying.",
        @"PACKAGE_MANAGER_STUCK": @"Linux first-start setup stopped making progress. Restart LinuxRunner and try again.",
        @"FILESYSTEM_ERROR": @"Linux could not write its files. Restart LinuxRunner and try again.",
        @"PLASMA_X11_UNAVAILABLE": @"These Arch repositories do not provide the Plasma X11 session required by LinuxRunner. Choose LinuxRunner Desktop. Wayland is not supported.",
        @"ACCOUNT_TOOL_BROKEN": @"The account tools were installed but do not run. Export the Open Desktop Report and send it.",
    };
    NSString *base = advice[category] ?: @"The installation failed.";
    return detail.length ? [NSString stringWithFormat:@"%@\n\nDetail: %@", base, detail] : base;
}
+ (void)postInstall:(NSString *)title phase:(NSString *)phase stage:(NSString *)stage detail:(NSString *)detail fraction:(double)fraction elapsed:(NSTimeInterval)elapsed error:(NSString *)error log:(NSString *)log {
    NSMutableDictionary *info = [@{@"title": title, @"phase": phase, @"stage": stage ?: @"", @"detail": detail ?: @"", @"fraction": @(fraction), @"elapsed": @(elapsed)} mutableCopy];
    if (error) info[@"error"] = error;
    if (log) info[@"log"] = log;
    [NSNotificationCenter.defaultCenter postNotificationName:LRInstallProgressNotification object:nil userInfo:info];
}
+ (void)runSupportInstall:(NSArray<NSString *> *)arguments title:(NSString *)title progress:(void (^)(NSString *))progress completion:(void (^)(NSError * _Nullable))completion {
    if (LRActiveInstall) { completion(lrError(EBUSY, [NSString stringWithFormat:@"“%@” is already running. Wait for it to finish, then try again.", LRActiveInstall])); return; }
    NSError *error = nil; NSURL *support = [LRGUIViewController prepareSupport:&error];
    if (!support) { completion(error); return; }
    for (NSString *name in @[@"install-result", @"apt-status"]) [NSFileManager.defaultManager removeItemAtURL:[support URLByAppendingPathComponent:name] error:nil];
    NSURL *logURL = [support URLByAppendingPathComponent:@"desktop-profile.sh.log"], *resultURL = [support URLByAppendingPathComponent:@"install-result"], *statusURL = [support URLByAppendingPathComponent:@"apt-status"];
    [NSFileManager.defaultManager removeItemAtURL:logURL error:nil];
    TerminalViewController *installer = [self terminalForCommand:[@[@"/bin/sh", @"/home/mobile/Downloads/.linuxrunner/gui/installer-run.sh", @"desktop-profile.sh"] arrayByAddingObjectsFromArray:arguments] directory:@"/root"];
    if (!installer) { completion(lrError(EIO, @"The installer could not be started. Restart LinuxRunner and try again.")); return; }
    LRActiveInstall = title;
    [self beginAwake:@"installation"];
    [self logCategory:@"SETUP" event:[NSString stringWithFormat:@"support install started: %@ (%@)", title, [arguments componentsJoinedByString:@" "]]];
    NSDate *start = NSDate.date; __block NSDate *endedAt = nil; __block NSString *reported = nil;
    [self postInstall:title phase:@"running" stage:@"Checking existing installation" detail:@"" fraction:-1 elapsed:0 error:nil log:nil];
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *timer) {
        NSString *log = [NSString stringWithContentsOfURL:logURL encoding:NSUTF8StringEncoding error:nil] ?: @"";
        NSString *stage = nil, *detail = @"", *category = nil, *failureDetail = nil; long done = -1, total = -1;
        for (NSString *line in [log componentsSeparatedByString:@"\n"]) {
            if ([line hasPrefix:@"PROFILE_STAGE: "]) { stage = [line substringFromIndex:15]; detail = @""; done = total = -1; }
            else if ([line hasPrefix:@"PACKAGE_STAGE: "]) {
                NSString *text = [line substringFromIndex:15]; NSRange colon = [text rangeOfString:@": "];
                stage = colon.location == NSNotFound ? text : [text substringToIndex:colon.location];
                detail = colon.location == NSNotFound ? @"" : [text substringFromIndex:NSMaxRange(colon)];
            } else if ([line hasPrefix:@"PACKAGE_PROGRESS: "]) {
                NSArray *parts = [[line substringFromIndex:18] componentsSeparatedByString:@" "];
                if (parts.count == 2) { done = [parts[0] integerValue]; total = [parts[1] integerValue]; }
            } else if ([line hasPrefix:@"PROFILE_FAILURE: "] || [line hasPrefix:@"PACKAGE_LOCK: category="]) {
                NSRange c = [line rangeOfString:@"category="]; NSString *rest = [line substringFromIndex:NSMaxRange(c)];
                NSRange space = [rest rangeOfString:@" "]; category = space.location == NSNotFound ? rest : [rest substringToIndex:space.location];
                NSRange d = [rest rangeOfString:@"detail="]; failureDetail = d.location == NSNotFound ? (space.location == NSNotFound ? @"" : [rest substringFromIndex:space.location + 1]) : [rest substringFromIndex:NSMaxRange(d)];
            }
        }
        double fraction = -1;
        if (done >= 0 && total > 0) fraction = MIN(1.0, (double)done / total);
        else if (stage && ([stage hasPrefix:@"Installing"] || [stage hasPrefix:@"Downloading"])) {
            // apt: "dlstatus:<n>:<percent>:<message>" / "pmstatus:<package>:<percent>:<message>"
            NSString *status = [[[NSString stringWithContentsOfURL:statusURL encoding:NSUTF8StringEncoding error:nil] ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsSeparatedByString:@"\n"].lastObject;
            NSArray *f = [status componentsSeparatedByString:@":"];
            if (f.count >= 4 && ([f[0] isEqual:@"dlstatus"] || [f[0] isEqual:@"pmstatus"])) {
                fraction = MAX(0, MIN(1.0, [f[2] doubleValue] / 100.0));
                detail = [[f subarrayWithRange:NSMakeRange(3, f.count - 3)] componentsJoinedByString:@":"];
                if ([f[0] isEqual:@"dlstatus"] && [stage hasPrefix:@"Installing"]) detail = [@"Downloading: " stringByAppendingString:detail];
            }
        }
        NSString *result = [[NSString stringWithContentsOfURL:resultURL encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (installer.sessionPid <= 0 && !endedAt) endedAt = NSDate.date;
        NSTimeInterval elapsed = -[start timeIntervalSinceNow];
        BOOL finished = result.length || (endedAt && -[endedAt timeIntervalSinceNow] > 5) || elapsed > 3600;
        if (finished) {
            [timer invalidate]; (void)installer;
            LRActiveInstall = nil;
            [self endAwake:@"installation"];
            [self logCategory:@"SETUP" event:[NSString stringWithFormat:@"support install finished: %@ result=%@ stage=%@ category=%@ seconds=%.0f", title, result ?: @"none", stage ?: @"none", category ?: @"none", elapsed]];
            if ([result isEqual:@"0"]) {
                [self postInstall:title phase:@"succeeded" stage:@"Installation complete" detail:@"" fraction:1 elapsed:elapsed error:nil log:nil];
                completion(nil);
                return;
            }
            NSString *message = category ? LRInstallFailureMessage(category, failureDetail) :
                result.length ? [NSString stringWithFormat:@"The installation failed at “%@”. Export the Open Desktop Report and try again.", stage ?: @"start"] :
                endedAt ? @"The installer stopped before finishing. Try again; if it repeats, export the Open Desktop Report." :
                @"The installation did not finish within an hour. Restart LinuxRunner and try again.";
            NSArray *lines = [log componentsSeparatedByString:@"\n"];
            NSString *tail = [[lines subarrayWithRange:NSMakeRange(lines.count > 25 ? lines.count - 25 : 0, MIN((NSUInteger)25, lines.count))] componentsJoinedByString:@"\n"];
            [self postInstall:title phase:@"failed" stage:stage ?: @"Starting" detail:detail fraction:-1 elapsed:elapsed error:message log:tail];
            completion(lrError(EIO, message));
            return;
        }
        if (stage && ![stage isEqual:reported]) { reported = stage; progress(stage); }
        [self postInstall:title phase:@"running" stage:stage ?: @"Waiting for the package manager" detail:detail fraction:fraction elapsed:elapsed error:nil log:nil];
    }];
}

// ---- Keep Screen Awake During Operations ----------------------------------------
static NSCountedSet<NSString *> *LRAwakeSet;
+ (void)beginAwake:(NSString *)reason {
    dispatch_async(dispatch_get_main_queue(), ^{ if (!LRAwakeSet) LRAwakeSet = [NSCountedSet new]; [LRAwakeSet addObject:reason]; [self refreshAwake]; });
}
+ (void)endAwake:(NSString *)reason {
    dispatch_async(dispatch_get_main_queue(), ^{ if ([LRAwakeSet countForObject:reason]) [LRAwakeSet removeObject:reason]; [self refreshAwake]; });
}
+ (NSArray<NSString *> *)awakeReasons { return LRAwakeSet.allObjects ?: @[]; }
+ (void)refreshAwake {
    dispatch_async(dispatch_get_main_queue(), ^{
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            [NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { [self refreshAwake]; }];
        });
        NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
        BOOL enabled = [defaults objectForKey:@"LinuxRunner.keepScreenAwake"] ? [defaults boolForKey:@"LinuxRunner.keepScreenAwake"] : YES;
        BOOL keep = UserPreferences.shared.shouldDisableDimming || (enabled && LRAwakeSet.count > 0);
        if (UIApplication.sharedApplication.idleTimerDisabled != keep) {
            UIApplication.sharedApplication.idleTimerDisabled = keep;
            [self logCategory:@"POWER" event:[NSString stringWithFormat:@"screen awake=%d setting=%d reasons=%@", keep, enabled, [LRAwakeSet.allObjects componentsJoinedByString:@","] ?: @""]];
        }
    });
}
+ (TerminalViewController *)changeLinuxPassword:(NSString *)name error:(NSError **)error {
    if (!LRLoginAccount(name)) { if(error)*error=lrError(ENOENT,@"Refresh the account list first."); return nil; }
    NSString *passwd=LRAccountTool(@[@"/usr/bin/passwd", @"/bin/passwd"]);
    if (!passwd) { if(error)*error=lrError(ENOENT,@"The guest passwd program is not installed."); return nil; }
    return [self terminalForCommand:@[passwd,name] directory:@"/"];
}
+ (TerminalViewController *)removeLinuxUserKeepingHome:(NSString *)name error:(NSError **)error {
    NSDictionary *account = LRLoginAccount(name);
    if (!account) { if(error)*error=lrError(ENOENT,@"Refresh the account list first."); return nil; }
    if ([account[@"uid"] longLongValue] < 1000 || [name isEqual:self.defaultLinuxUser]) {
        if(error)*error=lrError(EPERM,@"System accounts and the default login cannot be removed. Select a different default login first.");
        return nil;
    }
    BOOL shadow = self.activeEnvironmentIsDebian || self.activeEnvironmentIsArch;
    NSString *remove = LRAccountTool(shadow ? @[@"/usr/sbin/userdel", @"/usr/bin/userdel"] :
        @[@"/usr/sbin/deluser", @"/sbin/deluser", @"/usr/bin/deluser", @"/bin/deluser"]);
    if (!remove) { if(error)*error=lrError(ENOENT,@"The distribution's account-removal tool is missing. Install it using Package Center."); return nil; }
    // No --force / --remove-home: the real guest tool owns account locks,
    // active-process checks and passwd/group/shadow updates. Keep user files.
    NSArray *command = shadow ? @[remove,@"--",name] : @[remove,name];
    return [self terminalForCommand:command directory:@"/"];
}
+ (TerminalViewController *)terminalForCommand:(NSArray<NSString *> *)command directory:(NSString *)directory {
    TerminalViewController *vc = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    vc.runnerCommand = command; vc.runnerDirectory = directory; vc.runnerReportsExit = YES;
    // Attach the terminal view before a fast command can exit. Otherwise a
    // detached renderer can initialize at its oversized off-screen defaults.
    [vc loadViewIfNeeded];
    [vc startNewSession];
    return vc;
}
+ (NSURL *)prepareApplicationSupport:(NSError **)error { return [LRGUIViewController prepareSupport:error]; }
+ (void)resetGraphicalSessions {
    NSURL *base=[[self exchangeURL:self.activeRootName] URLByAppendingPathComponent:@".linuxrunner/gui"];
    for(NSURL *url in [NSFileManager.defaultManager contentsOfDirectoryAtURL:base includingPropertiesForKeys:@[NSURLIsDirectoryKey,NSURLIsSymbolicLinkKey] options:0 error:nil]) {
        if(![url.lastPathComponent hasPrefix:@"session-"])continue;
        NSDictionary *values=[url resourceValuesForKeys:@[NSURLIsDirectoryKey,NSURLIsSymbolicLinkKey] error:nil];
        if(![values[NSURLIsDirectoryKey] boolValue]||[values[NSURLIsSymbolicLinkKey] boolValue])continue;
        [NSData.data writeToURL:[url URLByAppendingPathComponent:@"stop"] options:NSDataWritingAtomic error:nil];
    }
    [self logCategory:@"GUI" event:@"owned-session stop markers requested; cleanup remains session-owned"];
}
// ---- Open Desktop attempt timeline ------------------------------------------
// One correlation id per Open Desktop request; every stage from the button to
// the first painted frame records start/ok/fail with a monotonic offset, the
// thread and a short detail. Kept in memory (last 400 records) and appended to
// Documents/Diagnostics/open-desktop-trace.log so a failed attempt survives a
// relaunch. Details are fixed texts, states and counters: no passwords, tokens,
// cookies or user file contents.
static NSMutableArray<NSString *> *desktopTraceRecords;
static NSString *desktopTraceAttempt;
static uint64_t desktopTraceOrigin;
static double LRTraceMilliseconds(uint64_t from) {
    static mach_timebase_info_data_t base; if (!base.denom) mach_timebase_info(&base);
    return (double)(mach_absolute_time() - from) * base.numer / base.denom / 1e6;
}
static NSURL *LRDesktopTraceFile(void) {
    NSURL *folder = [[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"Diagnostics" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
    return [folder URLByAppendingPathComponent:@"open-desktop-trace.log"];
}
static void LRDesktopTraceAppend(NSString *line) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("linuxrunner.desktop-trace", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(queue, ^{
        NSURL *url = LRDesktopTraceFile();
        NSNumber *size = nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        if (size.longLongValue > 512 * 1024) { // keep the newer half
            NSData *data = [NSData dataWithContentsOfURL:url]; if (data.length > 256 * 1024) [[data subdataWithRange:NSMakeRange(data.length - 256 * 1024, 256 * 1024)] writeToURL:url atomically:YES];
        }
        NSFileHandle *handle = [NSFileHandle fileHandleForWritingToURL:url error:nil];
        if (!handle) { [NSData.data writeToURL:url atomically:YES]; handle = [NSFileHandle fileHandleForWritingToURL:url error:nil]; }
        [handle seekToEndOfFile]; [handle writeData:[[line stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]]; [handle closeFile];
    });
}
+ (NSString *)desktopTraceBegin:(NSString *)source {
    @synchronized(self) {
        desktopTraceAttempt = [[NSUUID.UUID.UUIDString substringToIndex:8] lowercaseString];
        desktopTraceOrigin = mach_absolute_time();
    }
    [self desktopTrace:@"01 button" result:@"ok" detail:[NSString stringWithFormat:@"source=%@ runtime=%@ root=%@ app-state=%ld", source, self.isRunning ? @"running" : @"not-running", self.activeRootName, (long)UIApplication.sharedApplication.applicationState]];
    return desktopTraceAttempt;
}
+ (NSString *)desktopTraceAttempt { @synchronized(self) { return desktopTraceAttempt ?: @"none"; } }
+ (void)desktopTrace:(NSString *)stage result:(NSString *)result detail:(NSString *)detail {
    NSString *thread = NSThread.isMainThread ? @"main" : [NSString stringWithFormat:@"thread-%u", pthread_mach_thread_np(pthread_self())];
    NSString *line;
    @synchronized(self) {
        if (!desktopTraceAttempt) { desktopTraceAttempt = @"no-button"; desktopTraceOrigin = mach_absolute_time(); }
        static NSDateFormatter *clock; if (!clock) { clock = [NSDateFormatter new]; clock.dateFormat = @"HH:mm:ss.SSS"; }
        line = [NSString stringWithFormat:@"attempt=%@ +%.0fms %@ %@ stage=%@ result=%@%@%@", desktopTraceAttempt, LRTraceMilliseconds(desktopTraceOrigin),
                [clock stringFromDate:NSDate.date], thread, stage, result, detail.length ? @" " : @"", detail ?: @""];
        if (!desktopTraceRecords) desktopTraceRecords = [NSMutableArray new];
        [desktopTraceRecords addObject:line]; if (desktopTraceRecords.count > 400) [desktopTraceRecords removeObjectAtIndex:0];
    }
    LRDesktopTraceAppend(line);
    LRBootEvent([[@"[DESKTOP_TRACE] " stringByAppendingString:line] UTF8String]);
}
+ (NSArray<NSString *> *)desktopTraceRecords { @synchronized(self) { return desktopTraceRecords.copy ?: @[]; } }
+ (NSString *)desktopTraceSavedText {
    return [NSString stringWithContentsOfURL:LRDesktopTraceFile() encoding:NSUTF8StringEncoding error:nil] ?: @"";
}
+ (NSDictionary *)inputDiagnostics { return [LRGUIViewController latestInputDiagnostics]; }
+ (void)exportDesktopReport:(void (^)(NSURL * _Nullable))completion {
    [LRGUIViewController buildDesktopReport:^(NSString *text) {
        NSURL *url = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"LinuxRunner-OpenDesktop-%@.txt", self.desktopTraceAttempt]]];
        completion([text writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:nil] ? url : nil);
    }];
}
+ (void)logCategory:(NSString *)category event:(NSString *)event {
    // Callers pass fixed messages or numeric diagnostics, never argv, output,
    // document names, host paths, register values, or guest memory contents.
    LRBootEvent([[NSString stringWithFormat:@"[%@] %@", category, event] UTF8String]);
    @synchronized(self) {
        if (!events) events = [NSMutableArray new];
        [events addObject:[NSString stringWithFormat:@"%@ [%@] %@", [[NSISO8601DateFormatter new] stringFromDate:NSDate.date], category, event]];
        if (events.count > 500) [events removeObjectAtIndex:0];
    }
}
+ (NSArray *)logLines { @synchronized(self) { return events.copy ?: @[]; } }
+ (NSURL *)exportLogs:(NSError **)error {
    NSURL *url = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
                [NSString stringWithFormat:@"LinuxRunner-diagnostics-%@.txt", NSUUID.UUID.UUIDString]]];
    NSDictionary *jit = self.jitStatusReport, *gpu = self.gpuStatus;
    NSString *header = [NSString stringWithFormat:@"LinuxRunner %@ (%@)\nLinuxRunner compatibility runtime; ARM64\nJIT: %@ · %@ (mode %@, debugger %@, blocks %@)\nGPU: %@ · %@ (mode %@)\n%@\n",
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
        [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"],
        jit[@"state"], jit[@"detail"], jit[@"mode"], [jit[@"debugger"] boolValue] ? @"attached" : @"none", jit[@"blocks"] ?: @0,
        gpu[@"active"], gpu[@"detail"], gpu[@"mode"], self.runtimeStatus];
    if (bootSnapshot) msync(bootSnapshot, sizeof(*bootSnapshot), MS_SYNC);
    NSMutableString *text = [header mutableCopy];
    [text appendFormat:@"Device diagnostics: %@\n", [self deviceDiagnostics]];
    struct proc_data pi = {0};
    futex_pi_show(&pi);
    NSString *piText = [[NSString alloc] initWithBytes:pi.data length:pi.size encoding:NSUTF8StringEncoding];
    [text appendFormat:@"\n--- PI futex snapshot ---\n%@", piText ?: @"unavailable\n"];
    free(pi.data);
    for (NSString *name in @[@"previous-startup.log", @"startup.log"])
        [text appendFormat:@"\n--- %@ ---\n%@", name, [NSString stringWithContentsOfURL:[bootLogDirectory URLByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil] ?: @"unavailable\n"];
    if (bootSnapshot) [text appendFormat:@"\nLast observed guest context (best effort): pid=%llu pc=%llx sp=%llx syscall=%llu unsupported-opcode=%llx\n",
        (unsigned long long)atomic_load(&bootSnapshot->pid), (unsigned long long)atomic_load(&bootSnapshot->pc), (unsigned long long)atomic_load(&bootSnapshot->sp), (unsigned long long)atomic_load(&bootSnapshot->syscall), (unsigned long long)atomic_load(&bootSnapshot->opcode)];
    return [[self redactDiagnosticText:text application:NO] writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:error] ? url : nil;
}
// Explicit allowlist: never archive profiles, cookies, keys, authentication
// files, memory snapshots, or an arbitrary guest filesystem subtree.
+ (NSURL *)exportFullDiagnosticBundle:(NSError **)error {
    [LRDiagnosticCenter flush];
    NSMutableDictionary<NSString *,NSData *> *entries=[NSMutableDictionary new];
    entries[@"health.json"]=[NSJSONSerialization dataWithJSONObject:self.healthSummary options:NSJSONWritingPrettyPrinted error:nil];
    entries[@"events.json"]=[NSJSONSerialization dataWithJSONObject:self.diagnosticEntries options:NSJSONWritingPrettyPrinted error:nil];
    NSURL *installerLogs=[[[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject URLByAppendingPathComponent:@"LinuxRunnerDiagnostics"] URLByAppendingPathComponent:@"debian-installer"];
    for(NSString *name in @[@"events.jsonl",@"events.1.jsonl",@"events.2.jsonl"]){
        NSURL *path=[installerLogs URLByAppendingPathComponent:name];int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);if(fd<0)continue;struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);continue;}
        off_t offset=st.st_size>1024*1024?st.st_size-1024*1024:0;NSMutableData *data=[NSMutableData dataWithLength:st.st_size-offset];ssize_t n=pread(fd,data.mutableBytes,data.length,offset);close(fd);if(n<0)continue;data.length=n;
        NSString *text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding]?:@"non-UTF8 installer output omitted";
        entries[[@"debian-installer/" stringByAppendingString:name]]=[[self redactDiagnosticText:text application:NO] dataUsingEncoding:NSUTF8StringEncoding];
    }
    NSDictionary *catalog=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:self.catalogURL]?:[NSData data] options:0 error:nil];
    NSMutableArray *safeEnvironments=[NSMutableArray new];
    for(NSDictionary *item in catalog[@"environments"]){NSMutableDictionary *safe=[NSMutableDictionary new];for(NSString *key in @[@"id",@"distribution",@"distributionID",@"architecture",@"rootfsName",@"rootfsVersion",@"packageManager",@"lastBootedAt"]){if(item[key])safe[key]=item[key];}[safeEnvironments addObject:safe];}
    entries[@"environments.json"]=[NSJSONSerialization dataWithJSONObject:safeEnvironments options:NSJSONWritingPrettyPrinted error:nil];
    for(NSString *name in @[@"startup.log",@"previous-startup.log"]){NSString *text=[NSString stringWithContentsOfURL:[bootLogDirectory URLByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil];if(text)entries[name]=[[self redactDiagnosticText:text application:NO] dataUsingEncoding:NSUTF8StringEncoding];}
    for(NSString *root in self.rootNames){
        NSURL *base=[[[self exchangeURL:root] URLByAppendingPathComponent:@".linuxrunner"] URLByAppendingPathComponent:@"gui"];
        for(NSString *name in @[@"package-operation.log",@"package-operation.previous.log",@"package-result",@"package-interrupted",@"install.sh.log",@"install-qt.sh.log",@"install-browser.sh.log"]){
            NSURL *url=[base URLByAppendingPathComponent:name];int fd=open(url.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);if(fd<0)continue;struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);continue;}
            off_t offset=st.st_size>1024*1024?st.st_size-1024*1024:0;NSMutableData *data=[NSMutableData dataWithLength:st.st_size-offset];ssize_t n=pread(fd,data.mutableBytes,data.length,offset);close(fd);if(n<0)continue;data.length=n;
            NSString *text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding]?:@"non-UTF8 package output omitted";
            entries[[NSString stringWithFormat:@"packages/%@/%@",root,name]]=[[self redactDiagnosticText:text application:NO] dataUsingEncoding:NSUTF8StringEncoding];
        }
        NSArray<NSURL *> *children=[NSFileManager.defaultManager contentsOfDirectoryAtURL:base includingPropertiesForKeys:@[NSURLIsSymbolicLinkKey,NSURLContentModificationDateKey] options:0 error:nil];
        NSMutableArray<NSURL *> *sessions=[NSMutableArray new];
        for(NSURL *u in children)if([u.lastPathComponent hasPrefix:@"session-"]){NSNumber *link=nil;[u getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:nil];if(!link.boolValue)[sessions addObject:u];}
        [sessions sortUsingComparator:^NSComparisonResult(NSURL *a,NSURL *b){NSDate *ad=nil,*bd=nil;[a getResourceValue:&ad forKey:NSURLContentModificationDateKey error:nil];[b getResourceValue:&bd forKey:NSURLContentModificationDateKey error:nil];return [(bd?:NSDate.distantPast) compare:(ad?:NSDate.distantPast)];}];
        for(NSURL *session in [sessions subarrayWithRange:NSMakeRange(0,MIN(sessions.count,3))])for(NSString *name in @[@"session.log",@"application.log",@"focus.log",@"window.log",@"display.log",@"xvfb.log",@"bus.log",@"window-state",@"wm.log",@"panel.log",@"desktop.log",@"plasma.log",@"xfconf.log",@"desktop-state"]){
            NSURL *url=[session URLByAppendingPathComponent:name];int fd=open(url.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW);if(fd<0)continue;struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);continue;}
            off_t offset=st.st_size>1024*1024?st.st_size-1024*1024:0;NSMutableData *data=[NSMutableData dataWithLength:st.st_size-offset];ssize_t n=pread(fd,data.mutableBytes,data.length,offset);close(fd);if(n<0)continue;data.length=n;
            NSString *text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding]?:@"non-UTF8 application output omitted";
            BOOL applicationOutput=[@[@"application.log",@"wm.log",@"panel.log",@"desktop.log",@"plasma.log",@"xfconf.log"] containsObject:name];
            NSString *safe=[self redactDiagnosticText:text application:applicationOutput];
            entries[[NSString stringWithFormat:@"gui/%@/%@/%@",root,session.lastPathComponent,name]]=[safe dataUsingEncoding:NSUTF8StringEncoding];
        }
    }
    NSURL *url=[NSFileManager.defaultManager.temporaryDirectory URLByAppendingPathComponent:[NSString stringWithFormat:@"LinuxRunner-Diagnostics-%@.zip",NSUUID.UUID.UUIDString]];
    struct archive *archive=archive_write_new();archive_write_set_format_zip(archive);
    int result=archive_write_open_filename(archive,url.fileSystemRepresentation);
    if(result==ARCHIVE_OK)for(NSString *name in entries){NSData *data=entries[name];struct archive_entry *entry=archive_entry_new();archive_entry_set_pathname(entry,name.UTF8String);archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,data.length);result=archive_write_header(archive,entry);if(result==ARCHIVE_OK&&archive_write_data(archive,data.bytes,data.length)!=(la_ssize_t)data.length)result=ARCHIVE_FATAL;archive_entry_free(entry);if(result!=ARCHIVE_OK)break;}
    if(archive_write_close(archive)!=ARCHIVE_OK)result=ARCHIVE_FATAL;archive_write_free(archive);
    if(result!=ARCHIVE_OK){[NSFileManager.defaultManager removeItemAtURL:url error:nil];if(error)*error=lrError(EIO,@"Diagnostic archive creation failed");return nil;}
    return url;
}
+ (NSURL *)sharedExchangeURL:(NSString *)root {
    NSParameterAssert(validRoot(root));
    NSURL *documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    return [[documents URLByAppendingPathComponent:@"Shared" isDirectory:YES] URLByAppendingPathComponent:root isDirectory:YES];
}
static BOOL LRTransferString(NSString *value){return value&&strlen(value.UTF8String)==[value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];}
static int LRTransferStaging(void) {
    static dispatch_once_t once;static NSURL *url;static int failure;
    dispatch_once(&once,^{
        NSURL *support=[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
        url=[support URLByAppendingPathComponent:@"LinuxRunnerTransferStaging" isDirectory:YES];
        if(![NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil]){failure=EIO;return;}
        int fd=open(url.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
        failure=fd<0?errno:lr_clean_import_staging(fd);if(fd>=0)close(fd);
    });
    if(failure){errno=failure;return -1;}
    return open(url.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
}
static int LRSharedDirectory(NSString *root,NSString *relative){
    if(!validRoot(root)||!LRTransferString(relative)){errno=EINVAL;return -1;}
    NSURL *base=[LRRuntimeBridge sharedExchangeURL:root];
    NSError *error=nil;if(![NSFileManager.defaultManager createDirectoryAtURL:base withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]){errno=EIO;return -1;}
    // Both app-owned ancestors must remain directories, never symbolic links.
    int parent=open(base.URLByDeletingLastPathComponent.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(parent<0)return -1;
    int directory=openat(parent,root.UTF8String,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);int saved=errno;close(parent);if(directory<0){errno=saved;return -1;}
    int result=lr_open_subdirectory(directory,relative.UTF8String);saved=errno;close(directory);errno=saved;return result;
}
+ (NSArray<NSDictionary *> *)sharedEntries:(NSString *)relative root:(NSString *)root error:(NSError **)error {
    int fd=LRSharedDirectory(root,relative);if(fd<0){if(error)*error=lrError(errno,@"Cannot open the shared folder. Symbolic links are not followed.");return nil;}
    DIR *directory=fdopendir(fd);if(!directory){int saved=errno;close(fd);if(error)*error=lrError(saved,@"Cannot list this folder.");return nil;}
    NSMutableArray *items=[NSMutableArray new];struct dirent *entry;int failure=0;
    for(;;){errno=0;entry=readdir(directory);if(!entry){failure=errno;break;}if(entry->d_name[0]=='.')continue;
        struct stat st;if(fstatat(fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW)<0)continue;
        if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))continue;
        NSString *name=[NSString stringWithUTF8String:entry->d_name];if(!name)continue;
        [items addObject:@{@"name":name,@"directory":@(S_ISDIR(st.st_mode)),@"bytes":@(st.st_size)}];if(items.count>4096){failure=EFBIG;break;}
    }
    closedir(directory);if(failure){if(error)*error=lrError(failure,@"Folder could not be listed completely (maximum 4096 entries).");return nil;}
    [items sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){if([a[@"directory"] boolValue]!=[b[@"directory"] boolValue])return [a[@"directory"] boolValue]?NSOrderedAscending:NSOrderedDescending;return [a[@"name"] localizedStandardCompare:b[@"name"]];}];return items;
}
+ (BOOL)createSharedDirectory:(NSString *)leaf relative:(NSString *)relative root:(NSString *)root error:(NSError **)error {
    if(!LRTransferString(leaf)||!lr_valid_leaf(leaf.UTF8String)){if(error)*error=lrError(EINVAL,@"Choose a single folder name.");return NO;}
    int fd=LRSharedDirectory(root,relative);int result=fd<0?-1:mkdirat(fd,leaf.UTF8String,0700);int saved=errno;if(fd>=0)close(fd);
    if(result<0&&error)*error=lrError(saved,@"Cannot create the folder; an existing item is never replaced.");return result==0;
}
+ (BOOL)importSharedFile:(NSURL *)source relative:(NSString *)relative root:(NSString *)root control:(LRTransferControl *)control error:(NSError **)error {
    BOOL scoped=[source startAccessingSecurityScopedResource];__block int result=0;
    NSFileCoordinator *coordinator=[[NSFileCoordinator alloc]initWithFilePresenter:nil];NSError *coordError=nil;
    struct lr_transfer_control callbacks=LRTransferCallbacks(control);
    [coordinator coordinateReadingItemAtURL:source options:0 error:&coordError byAccessor:^(NSURL *url){
        int input=open(url.fileSystemRepresentation,O_RDONLY|O_NONBLOCK|O_NOFOLLOW);int saved=errno;
        int directory=LRSharedDirectory(root,relative),directoryError=directory<0?errno:0;
        int stage=LRTransferStaging(),stageError=stage<0?errno:0;
        result=input<0?saved:directoryError?directoryError:stageError?stageError:!LRTransferString(source.lastPathComponent)?EINVAL:lr_copy_in_staged(input,directory,source.lastPathComponent.UTF8String,stage,&callbacks);
        if(input>=0)close(input);if(directory>=0)close(directory);if(stage>=0)close(stage);
    }];
    if(scoped)[source stopAccessingSecurityScopedResource];
    if(coordError||result){if(error)*error=coordError?:lrError(result,result==ECANCELED?@"Transfer cancelled; no destination file was published.":result==EEXIST?@"That name already exists. Choose a different folder or rename the source.":result==EBUSY?@"The source changed during copying. Close its editor and retry.":@"Import failed. Use a regular file up to 512 MiB; existing items and symbolic links are not replaced.");return NO;}
    [self logCategory:@"FILES" event:@"shared file import completed"];return YES;
}
+ (NSURL *)exportSharedItem:(NSString *)leaf relative:(NSString *)relative root:(NSString *)root control:(LRTransferControl *)control error:(NSError **)error {
    if(!LRTransferString(leaf)||!lr_valid_leaf(leaf.UTF8String)){if(error)*error=lrError(EINVAL,@"Invalid filename.");return nil;}
    int directory=LRSharedDirectory(root,relative);struct stat st;int failure=directory<0?errno:fstatat(directory,leaf.UTF8String,&st,AT_SYMLINK_NOFOLLOW)<0?errno:0;
    NSURL *folder=[NSFileManager.defaultManager.temporaryDirectory URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    if(!failure&&![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:error]){close(directory);return nil;}
    NSString *name=!failure&&S_ISDIR(st.st_mode)?[leaf stringByAppendingString:@".zip"]:leaf;NSURL *target=[folder URLByAppendingPathComponent:name];
    int output=failure?-1:open(target.fileSystemRepresentation,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(!failure&&output<0)failure=errno;
    struct lr_transfer_control callbacks=LRTransferCallbacks(control);
    if(!failure)failure=S_ISDIR(st.st_mode)?lr_archive_directory(directory,leaf.UTF8String,output,&callbacks):lr_copy_out_controlled(directory,leaf.UTF8String,output,&callbacks);
    if(output>=0&&close(output)<0&&!failure)failure=errno;if(directory>=0)close(directory);
    if(failure){[NSFileManager.defaultManager removeItemAtURL:folder error:nil];if(error)*error=lrError(failure,failure==ECANCELED?@"Export cancelled; the temporary copy was removed.":failure==EBUSY?@"The selected item changed during export. Close applications writing to it and retry.":@"Export failed. Use regular files or directories without links/special files, up to 512 MiB, 4096 entries and 32 directory levels.");return nil;}
    [self logCategory:@"FILES" event:@"shared export snapshot prepared"];return target;
}
+ (BOOL)importFile:(NSURL *)source root:(NSString *)root error:(NSError **)error {
    BOOL scoped = [source startAccessingSecurityScopedResource];
    __block int result = 0;
    NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
    NSError *coordError;
    [coordinator coordinateReadingItemAtURL:source options:0 error:&coordError byAccessor:^(NSURL *url) {
        int input = open(url.fileSystemRepresentation, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        int directory = open([self exchangeURL:root].fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (input < 0 || directory < 0) result = errno;
        else result = lr_copy_in(input, directory, source.lastPathComponent.UTF8String);
        if (input >= 0) close(input);
        if (directory >= 0) close(directory);
    }];
    if (scoped) [source stopAccessingSecurityScopedResource];
    if (coordError || result) {
        if (error) *error = coordError ?: lrError(result, result == EEXIST ? @"A file with that name already exists. Rename the source before importing." : @"Import failed. Only regular files up to 512 MiB are supported.");
        [self logCategory:@"FS" event:[NSString stringWithFormat:@"import failed code=%d", result]];
        return NO;
    }
    [self logCategory:@"FS" event:@"file imported"];
    return YES;
}
+ (NSURL *)exportFile:(NSString *)leaf root:(NSString *)root error:(NSError **)error {
    if (!lr_valid_leaf(leaf.UTF8String)) {
        if (error) *error = lrError(EINVAL, @"Invalid export filename.");
        return nil;
    }
    NSURL *folder = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString] isDirectory:YES];
    if (![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:NO attributes:nil error:error]) return nil;
    NSURL *url = [folder URLByAppendingPathComponent:leaf];
    int directory = open([self exchangeURL:root].fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    int output = open(url.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int result = directory < 0 || output < 0 ? errno : lr_copy_out(directory, leaf.UTF8String, output);
    if (directory >= 0) close(directory);
    if (output >= 0) close(output);
    if (result) {
        [NSFileManager.defaultManager removeItemAtURL:folder error:nil];
        if (error) *error = lrError(result, @"Export failed. Select a regular file up to 512 MiB; symlinks are refused.");
        return nil;
    }
    [self logCategory:@"FS" event:@"file exported"];
    return url;
}
+ (void)deleteExport:(NSURL *)url {
    // Cleanup is descriptor-relative to app temporary storage. Never follow a
    // replaced parent link to a shared source or a Files-provider destination.
    NSString *base=NSFileManager.defaultManager.temporaryDirectory.URLByStandardizingPath.path;
    NSURL *standard=url.URLByStandardizingPath;NSString *prefix=[base stringByAppendingString:@"/"];
    if(!url.isFileURL||![standard.path hasPrefix:prefix])return;
    NSString *relative=[standard.path substringFromIndex:prefix.length],*leaf=relative.lastPathComponent,*parent=relative.stringByDeletingLastPathComponent;
    if(!LRTransferString(leaf)||!lr_valid_leaf(leaf.UTF8String))return;
    int root=open(base.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(root<0)return;
    int directory=lr_open_subdirectory(root,[parent isEqual:@"."]?"":parent.UTF8String);
    if(directory>=0){struct stat st;if(!fstatat(directory,leaf.UTF8String,&st,AT_SYMLINK_NOFOLLOW)&&!S_ISDIR(st.st_mode))unlinkat(directory,leaf.UTF8String,0);close(directory);}
    if([[NSUUID alloc]initWithUUIDString:parent]&&[parent isEqual:parent.lastPathComponent])unlinkat(root,parent.UTF8String,AT_REMOVEDIR);
    close(root);
}

#if TARGET_OS_SIMULATOR
#include "../../tests/linuxrunner/desktop_files_contract.inc"
#endif
@end

@implementation LRDeviceDiagnosticsController
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.systemBackgroundColor;
    UIScrollView *scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    UIStackView *stack = [UIStackView new]; stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 14; stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20], [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:20], [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-20],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40]]];
    UILabel *title = [UILabel new]; title.text = @"LinuxRunner Device Diagnostics"; title.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle1]; title.numberOfLines = 0; [stack addArrangedSubview:title];
    UILabel *label = [UILabel new]; label.text = @"Runtime Auto Start (next launch)"; [stack addArrangedSubview:label];
    self.autoStartSwitch = [UISwitch new]; self.autoStartSwitch.accessibilityIdentifier = @"Runtime Auto Start"; self.autoStartSwitch.on = LRRuntimeBridge.autoStart;
    [self.autoStartSwitch addTarget:self action:@selector(toggleAutoStart) forControlEvents:UIControlEventValueChanged]; [stack addArrangedSubview:self.autoStartSwitch];
    self.details = [UILabel new]; self.details.numberOfLines = 0; self.details.font = [UIFont monospacedSystemFontOfSize:13 weight:UIFontWeightRegular]; self.details.accessibilityIdentifier = @"Device diagnostic details"; [stack addArrangedSubview:self.details];
    self.startButton = [self button:@"Start Linux Runtime" selector:@selector(startRuntime)]; [stack addArrangedSubview:self.startButton];
    self.managerButton = [self button:@"Open Environment Manager" selector:@selector(openManager)]; [stack addArrangedSubview:self.managerButton];
    UIButton *stop = [self button:@"Stop Runtime — requires full app relaunch" selector:NULL]; stop.enabled = NO; [stack addArrangedSubview:stop];
    [stack addArrangedSubview:[self button:@"Export Logs" selector:@selector(exportLogs)]];
    [stack addArrangedSubview:[self button:@"Refresh Diagnostics" selector:@selector(refresh)]];
    [self refresh];
}
- (UIButton *)button:(NSString *)title selector:(SEL)selector {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem]; [button setTitle:title forState:UIControlStateNormal];
    if (selector) [button addTarget:self action:selector forControlEvents:UIControlEventTouchUpInside];
    return button;
}
- (void)viewDidAppear:(BOOL)animated { [super viewDidAppear:animated]; LRBootEvent("UI attached: native diagnostics (Linux not required)"); [self refresh]; }
- (void)toggleAutoStart { [NSUserDefaults.standardUserDefaults setBool:self.autoStartSwitch.on forKey:@"LinuxRunnerRuntimeAutoStart"]; LRBootEvent(self.autoStartSwitch.on ? "runtime auto start enabled for next launch" : "runtime auto start disabled for next launch"); }
- (void)refresh {
    NSDictionary *info = LRRuntimeBridge.deviceDiagnostics; NSMutableString *text = [NSMutableString new];
    for (NSString *key in [info.allKeys sortedArrayUsingSelector:@selector(compare:)]) [text appendFormat:@"%@: %@\n\n", key, info[key]];
    self.details.text = text; self.startButton.enabled = !bootAttempted; self.managerButton.enabled = LRRuntimeBridge.isRunning;
}
- (void)startRuntime {
    self.startButton.enabled = NO; LRBootEvent("Start Linux Runtime pressed"); [self refresh];
    dispatch_async(dispatch_get_main_queue(), ^{
        [(AppDelegate *)UIApplication.sharedApplication.delegate startLinuxRuntime]; [self refresh];
    });
}
- (void)openManager {
    if (!LRRuntimeBridge.isRunning) return;
    TerminalViewController *terminal = [[UIStoryboard storyboardWithName:@"Terminal" bundle:nil] instantiateInitialViewController];
    [terminal loadViewIfNeeded]; [terminal startNewSession];
    self.view.window.rootViewController = [LinuxRunnerShell makeControllerWithTerminal:terminal];
    LRBootEvent("UI attached: environment manager after manual runtime start");
}
- (void)exportLogs {
    NSError *error; NSURL *url = [LRRuntimeBridge exportLogs:&error];
    if (!url) { self.details.text = error.localizedDescription; return; }
    UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
    share.popoverPresentationController.sourceView = self.view;
    share.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, 40, 1, 1);
    share.completionWithItemsHandler = ^(UIActivityType type, BOOL complete, NSArray *items, NSError *failure) { [NSFileManager.defaultManager removeItemAtURL:url error:nil]; };
    [self presentViewController:share animated:YES completion:nil];
}
@end
