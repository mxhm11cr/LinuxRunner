//
//  ViewController.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import "TerminalViewController.h"
#ifdef LINUXRUNNER
#import "LinuxRunner-Swift.h"
#endif
#import "AppDelegate.h"
#import "TerminalView.h"
#import "BarButton.h"
#import "ArrowBarButton.h"
#import "UserPreferences.h"
#import "AboutViewController.h"
#import "NSObject+SaneKVO.h"
#import "LinuxInterop.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/devices.h"
#ifdef LINUXRUNNER
#import "LinuxRunner/RuntimeBridge.h"
#endif

@interface TerminalViewController () <UIGestureRecognizerDelegate>

@property UITapGestureRecognizer *tapRecognizer;
#ifdef LINUXRUNNER
// Keep Screen Awake: held while this terminal is visible and a command (not the
// shell's prompt) owns its foreground; released when idle, hidden or gone.
@property NSTimer *awakeTimer;
@property BOOL holdsAwake;
#endif
@property (weak, nonatomic) IBOutlet TerminalView *termView;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *bottomConstraint;

@property (weak, nonatomic) IBOutlet UIButton *tabKey;
@property (weak, nonatomic) IBOutlet UIButton *controlKey;
@property (weak, nonatomic) IBOutlet UIButton *escapeKey;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barButtons;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barControls;

@property (weak, nonatomic) IBOutlet UIInputView *barView;
@property (weak, nonatomic) IBOutlet UIStackView *bar;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTop;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barBottom;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barLeading;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barTrailing;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barButtonWidth;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barHeight;

@property (weak, nonatomic) IBOutlet UIButton *infoButton;
@property (weak, nonatomic) IBOutlet UIButton *pasteButton;
@property (weak, nonatomic) IBOutlet UIButton *hideKeyboardButton;

@property int sessionPid;
@property (nonatomic) Terminal *sessionTerminal;

@property BOOL ignoreKeyboardMotion;
@property int runnerLaunchError;
@property (nonatomic) BOOL hasExternalKeyboard;

@end

@implementation TerminalViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    if (@available(iOS 15.0, *)) {
        self.bottomConstraint.active = NO;
        [self.termView.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor].active = YES;
    }

#if !ISH_LINUX
    int bootError = [AppDelegate bootError];
    if (bootError < 0) {
        NSString *message = [NSString stringWithFormat:@"could not boot"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", bootError];
        if (bootError == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
        [self showMessage:message subtitle:subtitle];
        NSLog(@"boot failed with code %d", bootError);
    }
#endif

    self.terminal = self.terminal;
    [self.termView becomeFirstResponder];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardWillChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardDidChangeFrameNotification
                 object:nil];
    [self _updateStyleFromPreferences:NO];
    
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad) {
        [self.bar removeArrangedSubview:self.hideKeyboardButton];
        [self.hideKeyboardButton removeFromSuperview];
    }
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        self.barHeight.constant = 36;
    } else {
        self.barHeight.constant = 43;
    }
    
    // SF Symbols is cool
    if (@available(iOS 13, *)) {
        [self.infoButton setImage:[UIImage systemImageNamed:@"gear"] forState:UIControlStateNormal];
        [self.pasteButton setImage:[UIImage systemImageNamed:@"doc.on.clipboard"] forState:UIControlStateNormal];
        [self.hideKeyboardButton setImage:[UIImage systemImageNamed:@"keyboard.chevron.compact.down"] forState:UIControlStateNormal];
        
        [self.tabKey setTitle:nil forState:UIControlStateNormal];
        [self.tabKey setImage:[UIImage systemImageNamed:@"arrow.right.to.line.alt"] forState:UIControlStateNormal];
        [self.controlKey setTitle:nil forState:UIControlStateNormal];
        [self.controlKey setImage:[UIImage systemImageNamed:@"control"] forState:UIControlStateNormal];
        [self.escapeKey setTitle:nil forState:UIControlStateNormal];
        [self.escapeKey setImage:[UIImage systemImageNamed:@"escape"] forState:UIControlStateNormal];
    }
    
    [UserPreferences.shared observe:@[@"hideStatusBar"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
}

- (void)awakeFromNib {
    [super awakeFromNib];
#if !ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(processExited:)
                                               name:ProcessExitedNotification
                                             object:nil];
#else
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(kernelPanicked:)
                                               name:KernelPanicNotification
                                             object:nil];
#endif
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self.termView becomeFirstResponder];
#ifdef LINUXRUNNER
    [self.awakeTimer invalidate];
    __weak typeof(self) weakSelf = self;
    self.awakeTimer = [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *timer) { [weakSelf updateAwake:YES]; }];
    [self updateAwake:YES];
#endif
    if (self.runnerLaunchError < 0) {
        [self reportLaunchError:self.runnerLaunchError];
        self.runnerLaunchError = 0;
    }
}

#ifdef LINUXRUNNER
- (NSString *)awakeReason { return [NSString stringWithFormat:@"terminal-%p", (__bridge void *)self]; }
- (void)updateAwake:(BOOL)visible {
    // A terminal started for a command (package operation, application) is busy
    // while that command lives; an interactive shell only while a command it
    // started owns the foreground (not while it waits at the prompt).
    NSArray<NSString *> *command = self.runnerCommand;
    BOOL shell = command.count == 0 || ([@[@"bash", @"sh", @"ash", @"dash", @"zsh", @"fish", @"login", @"su"] containsObject:command.firstObject.lastPathComponent] && ![command containsObject:@"-c"]);
    BOOL busy = visible && self.sessionPid > 0 && (!shell || [self.sessionTerminal foregroundCommandRunning]);
    if (busy == self.holdsAwake) return;
    self.holdsAwake = busy;
    if (busy) [LRRuntimeBridge beginAwake:self.awakeReason]; else [LRRuntimeBridge endAwake:self.awakeReason];
}
- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [self.awakeTimer invalidate]; self.awakeTimer = nil;
    [self updateAwake:NO];
}
- (void)dealloc {
    [_awakeTimer invalidate];
    if (_holdsAwake) [LRRuntimeBridge endAwake:[NSString stringWithFormat:@"terminal-%p", (__bridge void *)self]];
}
#endif

- (void)sendSessionInput:(NSString *)text {
    [self.sessionTerminal sendInput:[text dataUsingEncoding:NSUTF8StringEncoding]];
}

- (void)reportLaunchError:(int)code {
    NSString *reason;
    switch (code) {
        case _ENOENT: reason = @"The executable or its required ELF interpreter is missing. Check the command and installed packages."; break;
        case _ENOEXEC: reason = @"This file is not a supported ARM64 Linux executable or script."; break;
        case _EACCES: reason = @"The executable cannot be read or executed. Check guest permissions."; break;
        case _ENOMEM: reason = @"The runtime could not allocate enough memory."; break;
        case _E2BIG: reason = @"The command and arguments exceed the 4 KiB launch limit."; break;
        default: reason = @"The Linux runtime could not start this session. See diagnostics."; break;
    }
    [self showMessage:@"Launch failed" subtitle:[reason stringByAppendingFormat:@"\nRuntime error: %d", code]];
}

- (void)startNewSession {
#ifdef LINUXRUNNER
    if (![LRRuntimeBridge isRunning]) {
        [self showMessage:@"Environment unavailable" subtitle:[LRRuntimeBridge runtimeStatus]];
        return;
    }
#endif
    int err = [self startSession];
    if (err < 0) {
#ifdef LINUXRUNNER
        [LRRuntimeBridge logCategory:@"EXEC" event:[NSString stringWithFormat:@"launch failed code=%d", err]];
#endif
        NSLog(@"could not start session: %d", err);
        if (self.viewIfLoaded.window) [self reportLaunchError:err];
        else self.runnerLaunchError = err;
    }
}

- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid {
    self.sessionTerminal = [Terminal terminalWithUUID:uuid];
    if (self.sessionTerminal == nil)
        [self startNewSession];
}

- (NSUUID *)sessionTerminalUUID {
    return self.terminal.uuid;
}

- (int)startSession {
    NSArray<NSString *> *command = self.runnerCommand ?: UserPreferences.shared.launchCommand;
#ifdef LINUXRUNNER
    if (!self.runnerCommand && ([LRRuntimeBridge activeEnvironmentIsDebian] || [LRRuntimeBridge activeEnvironmentIsArch])) command = @[@"/bin/bash", @"-l"];
    if (!self.runnerCommand) {
        NSError *accountError = nil;
        NSArray<NSString *> *login = [LRRuntimeBridge defaultLinuxLoginCommand:&accountError];
        if (accountError) {
            [LRRuntimeBridge logCategory:@"ACCOUNT" event:@"Selected login account unavailable; refusing root fallback"];
            return _ENOENT;
        }
        if (login) command = login;
    }
#endif
    if (command.count == 0) {
        NSLog(@"terminal session launch command is empty");
        return _EINVAL;
    }
    size_t bytes = 1;
    for (NSString *argument in command) {
        size_t length = [argument lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        if (length + 1 > 4096 - bytes) return _E2BIG;
        if (strlen(argument.UTF8String) != length) return _EINVAL;
        bytes += length + 1;
    }
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"BOOT" event:@"shell starting: creating PTY and guest child"];
#endif
    NSLog(@"starting terminal session with command: %@", [command componentsJoinedByString:@" "]);

#if !ISH_LINUX
    int err = become_new_init_child();
    if (err < 0)
        return err;
    struct tty *tty;
    self.sessionTerminal = nil;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSAssert(IS_ERR(tty), @"tty should be error");
        task_discard_unstarted(current);
        current = NULL;
        return (int) PTR_ERR(tty);
    }
    self.sessionTerminal = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    tty_release(tty);
    if (err < 0) goto session_failed;

    if (self.runnerDirectory.length) {
        struct fd *directory = generic_open(self.runnerDirectory.UTF8String, O_RDONLY_ | O_DIRECTORY_, 0);
        if (IS_ERR(directory)) { err = PTR_ERR(directory); goto session_failed; }
        fs_chdir(current->fs, directory);
    }

#ifdef LINUXRUNNER
    // Both glibc distributions use the same terminal/login path. su - selects
    // the configured user's home; a default root login starts in its home too.
    if (!self.runnerDirectory.length && !self.runnerCommand &&
        ([LRRuntimeBridge activeEnvironmentIsDebian] || [LRRuntimeBridge activeEnvironmentIsArch])) {
        struct fd *home = generic_open("/root", O_RDONLY_ | O_DIRECTORY_, 0);
        if (!IS_ERR(home)) fs_chdir(current->fs, home);
    }
#endif

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0"
        "HOME=/root\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
#if defined(GUEST_ARM64)
        "PYTHONMALLOC=malloc\0"
#endif
        ;
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"SYSCALL" event:[NSString stringWithFormat:@"execve result=%d", err]];
#endif
    if (err < 0) goto session_failed;
    self.sessionPid = current->pid;
    NSLog(@"started terminal session pid %d on %@", self.sessionPid, stdioFile);
    err = task_start(current);
    if (err < 0) {
session_failed:
        task_discard_unstarted(current);
        current = NULL;
        self.sessionPid = 0;
        [self.sessionTerminal destroy];
        self.sessionTerminal = nil;
        return err;
    }
#ifdef LINUXRUNNER
    [LRRuntimeBridge logCategory:@"BOOT" event:@"shell scheduled: task_start succeeded (prompt readiness not inferred)"];
#endif
    current = NULL; // guest execution now owns the task; UI must not borrow it
#else
    const char *argv_arr[command.count + 1];
    for (NSUInteger i = 0; i < command.count; i++)
        argv_arr[i] = command[i].UTF8String;
    argv_arr[command.count] = NULL;
    const char *envp_arr[] = {
        "TERM=xterm-256color",
        NULL,
    };
    const char *const *argv = argv_arr;
    const char *const *envp = envp_arr;
    __block Terminal *terminal = nil;
    __block int sessionPid = 0;
    __block int err = 1;
    sync_do_in_workqueue(^(void (^done)(void)) {
        linux_start_session(argv[0], argv, envp, ^(int retval, int pid, nsobj_t term) {
            err = retval;
            if (term)
                terminal = CFBridgingRelease(term);
            sessionPid = pid;
            done();
        });
    });
    NSAssert(err <= 0, @"session start did not finish??");
    if (err < 0)
        return err;
    self.sessionTerminal = terminal;
    self.sessionPid = sessionPid;
    NSLog(@"started terminal session pid %d", self.sessionPid);
#endif
    return 0;
}

#if !ISH_LINUX
- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    if (pid != self.sessionPid)
        return;

    int code = [notif.userInfo[@"code"] intValue];
    NSLog(@"terminal session pid %d exited with code %d", pid, code);
    if (self.runnerReportsExit) {
        // Keep the rendered output visible after a one-shot command exits.
        NSString *status = (code & 0x7f) ?
            [NSString stringWithFormat:@"Process ended by signal %d%@", code & 0x7f,
             (code & 0x7f) == 4 ? @" (unsupported instruction; see diagnostics)" : @""] :
            [NSString stringWithFormat:@"Exit status: %d", (code >> 8) & 255];
        NSString *line = [NSString stringWithFormat:@"\r\n[%@]\r\n", status];
        NSData *data = [line dataUsingEncoding:NSUTF8StringEncoding];
        [self.sessionTerminal sendOutput:data.bytes length:(int)data.length];
        [self.sessionTerminal destroy];
        self.sessionPid = 0;
#ifdef LINUXRUNNER
        [LRRuntimeBridge logCategory:@"EXEC" event:status];
#endif
        return;
    }
    [self.sessionTerminal destroy];
    self.sessionTerminal = nil;
    current = NULL; // it's been freed
    [self startNewSession];
}
#endif

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Linux runtime stopped" message:notif.userInfo[@"message"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    });
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object == [UserPreferences shared]) {
        [self _updateStyleFromPreferences:YES];
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

- (void)_updateStyleFromPreferences:(BOOL)animated {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    NSTimeInterval duration = animated ? 0.1 : 0;
    [UIView animateWithDuration:duration animations:^{
        self.view.backgroundColor = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.backgroundColor];
        UIKeyboardAppearance keyAppearance = UserPreferences.shared.keyboardAppearance;
        self.termView.keyboardAppearance = keyAppearance;
        for (BarButton *button in self.barButtons) {
            button.keyAppearance = keyAppearance;
        }
        UIColor *tintColor = keyAppearance == UIKeyboardAppearanceLight ? UIColor.blackColor : UIColor.whiteColor;
        for (UIControl *control in self.barControls) {
            control.tintColor = tintColor;
        }
    }];
    UIView *oldBarView = self.termView.inputAccessoryView;
    if (UserPreferences.shared.hideExtraKeysWithExternalKeyboard && self.hasExternalKeyboard) {
        self.termView.inputAccessoryView = nil;
    } else {
        self.termView.inputAccessoryView = self.barView;
    }
    if (self.termView.inputAccessoryView != oldBarView && self.termView.isFirstResponder) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.ignoreKeyboardMotion = YES; // avoid infinite recursion
            [self.termView reloadInputViews];
            self.ignoreKeyboardMotion = NO;
        });
    }
}
- (void)_updateStyleAnimated {
    [self _updateStyleFromPreferences:YES];
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

- (void)keyboardDidSomething:(NSNotification *)notification {
    if (self.ignoreKeyboardMotion)
        return;

    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen *screen = self.view.window.screen ?: UIScreen.mainScreen;
    // notification.object is nil before iOS 16.1 and the correct UIScreen after iOS 16.1
    if ([notification.object isKindOfClass:UIScreen.class])
        screen = notification.object;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromCoordinateSpace:screen.coordinateSpace];
    if (CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    BOOL intersects = !CGRectIsNull(intersection) && !CGRectIsEmpty(intersection);
    self.hasExternalKeyboard = !intersects || intersection.size.height < 100;
    if (@available(iOS 15.0, *))
        return; // UIKit's layout guide handles keyboard movement and safe areas.

    keyboardFrame = intersects ? intersection : CGRectZero;
    CGFloat pad = self.view.safeAreaInsets.bottom;
    // The keyboard appears to be undocked. This means it can either be split or
    // truly floating. In the former case we want to keep the pad, but in the
    // latter we should fall back to the input accessory view instead of the
    // keyboard.
    if (intersects) {
        pad = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboardFrame);
    }
    if (intersects && pad != keyboardFrame.size.height && keyboardFrame.size.width != self.view.bounds.size.width) {
        pad = MAX(self.view.safeAreaInsets.bottom, self.termView.inputAccessoryView.frame.size.height);
    }
    self.bottomConstraint.constant = pad;

    BOOL initialLayout = self.termView.needsUpdateConstraints;
    [self.view setNeedsUpdateConstraints];
    if (!initialLayout) {
        // if initial layout hasn't happened yet, the terminal view is going to be at a really weird place, so animating it is going to look really bad
        NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
        NSNumber *curve = notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
        [UIView animateWithDuration:interval.doubleValue
                              delay:0
                            options:curve.integerValue << 16
                         animations:^{
                             [self.view layoutIfNeeded];
                         }
                         completion:nil];
    }
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    if ([segue.identifier isEqualToString:@"embed"]) {
        // You might want to check if this is your embed segue here
        // in case there are other segues triggered from this view controller.
        segue.destinationViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    // Hack to resolve a layering mismatch between the UI and preferences.
    if (@available(iOS 12.0, *)) {
        if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle) {
            // Ensure that the relevant things listening for this will update.
            UserPreferences.shared.colorScheme = UserPreferences.shared.colorScheme;
        }
    }
}

#pragma mark Bar

- (IBAction)showAbout:(id)sender {
#ifdef LINUXRUNNER
    // LinuxRunner's own Terminal & Keyboard settings (the inherited settings
    // storyboard is not part of LinuxRunner).
    if ([sender isKindOfClass:[UIGestureRecognizer class]] && ((UIGestureRecognizer *)sender).state != UIGestureRecognizerStateBegan) return;
    [self presentViewController:[LRNativeScreens terminalSettings] animated:YES completion:nil];
    return;
#endif
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    if ([sender isKindOfClass:[UIGestureRecognizer class]]) {
        UIGestureRecognizer *recognizer = sender;
        if (recognizer.state == UIGestureRecognizerStateBegan) {
            AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
            aboutViewController.includeDebugPanel = YES;
        } else {
            return;
        }
    }
    [self presentViewController:navigationController animated:YES completion:nil];
    [self.termView resignFirstResponder];
}

- (void)resizeBar {
    CGSize bar = self.barView.bounds.size;
    // set sizing parameters on bar
    // numbers stolen from iVim and modified somewhat
    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone) {
        // phone
        [self setBarHorizontalPadding:6 verticalPadding:6 buttonWidth:32];
    } else if (bar.width >= 450) {
        // wide ipad
        [self setBarHorizontalPadding:15 verticalPadding:8 buttonWidth:43];
    } else {
        // narrow ipad (slide over)
        [self setBarHorizontalPadding:10 verticalPadding:8 buttonWidth:36];
    }
    [UIView performWithoutAnimation:^{
        [self.barView layoutIfNeeded];
    }];
}

- (void)setBarHorizontalPadding:(CGFloat)horizontal verticalPadding:(CGFloat)vertical buttonWidth:(CGFloat)buttonWidth {
    self.barLeading.constant = self.barTrailing.constant = horizontal;
    self.barTop.constant = self.barBottom.constant = vertical;
    self.barButtonWidth.constant = buttonWidth;
}

- (IBAction)pressEscape:(id)sender {
    [self pressKey:@"\x1b"];
}
- (IBAction)pressTab:(id)sender {
    [self pressKey:@"\t"];
}
- (void)pressKey:(NSString *)key {
    [self.termView insertText:key];
}

- (IBAction)pressControl:(id)sender {
    self.controlKey.selected = !self.controlKey.selected;
}
    
- (IBAction)pressArrow:(ArrowBarButton *)sender {
    switch (sender.direction) {
        case ArrowUp: [self pressKey:[self.terminal arrow:'A']]; break;
        case ArrowDown: [self pressKey:[self.terminal arrow:'B']]; break;
        case ArrowLeft: [self pressKey:[self.terminal arrow:'D']]; break;
        case ArrowRight: [self pressKey:[self.terminal arrow:'C']]; break;
        case ArrowNone: break;
    }
}

- (void)switchTerminal:(UIKeyCommand *)sender {
    unsigned i = (unsigned) sender.input.integerValue;
    if (i == 7)
        self.terminal = self.sessionTerminal;
    else
        self.terminal = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
}

- (void)increaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize + 1;
}
- (void)decreaseFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = self.termView.effectiveFontSize - 1;
}
- (void)resetFontSize:(UIKeyCommand *)command {
    self.termView.overrideFontSize = 0;
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    static NSMutableArray<UIKeyCommand *> *commands = nil;
    if (commands == nil) {
        commands = [NSMutableArray new];
        for (unsigned i = 1; i <= 7; i++) {
            [commands addObject:
             [UIKeyCommand keyCommandWithInput:[NSString stringWithFormat:@"%d", i]
                                 modifierFlags:UIKeyModifierCommand|UIKeyModifierAlternate|UIKeyModifierShift
                                        action:@selector(switchTerminal:)]];
        }
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"+"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)
                      discoverabilityTitle:@"Increase Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"="
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(increaseFontSize:)]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"-"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(decreaseFontSize:)
                      discoverabilityTitle:@"Decrease Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@"0"
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(resetFontSize:)
                      discoverabilityTitle:@"Reset Font Size"]];
        [commands addObject:
         [UIKeyCommand keyCommandWithInput:@","
                             modifierFlags:UIKeyModifierCommand
                                    action:@selector(showAbout:)
                      discoverabilityTitle:@"Settings"]];
    }
    return commands;
}

- (void)setTerminal:(Terminal *)terminal {
    _terminal = terminal;
    self.termView.terminal = self.terminal;
}

- (void)setSessionTerminal:(Terminal *)sessionTerminal {
    if (_terminal == _sessionTerminal)
        self.terminal = sessionTerminal;
    _sessionTerminal = sessionTerminal;
}

@end

@interface BarView : UIInputView
@property (weak) IBOutlet TerminalViewController *terminalViewController;
@property (nonatomic) IBInspectable BOOL allowsSelfSizing;
@end
@implementation BarView
@dynamic allowsSelfSizing;

- (void)layoutSubviews {
    [self.terminalViewController resizeBar];
}

@end
