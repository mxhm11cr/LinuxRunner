//
//  Terminal.m
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import "Terminal.h"
#import "DelayedUITask.h"
#import "UserPreferences.h"
#include "LinuxInterop.h"
#include "fs/devices.h"
#include "fs/tty.h"

extern struct tty_driver ios_pty_driver;

#if !ISH_LINUX
typedef struct tty *tty_t;
#else
typedef struct linux_tty *tty_t;
#endif

@interface Terminal () <WKScriptMessageHandler> {
#if !ISH_LINUX
    lock_t _dataLock;
    cond_t _dataConsumed;
#endif
}

@property BOOL loaded;
@property (nonatomic) tty_t tty;
// lock with dataLock for !linux and @synchronized(self) for linux
@property (nonatomic) NSMutableData *pendingData;
// sending output is an asynchronous thing due to javascript, this is used to ensure it doesn't happen twice at once
@property (nonatomic) BOOL outputInProgress;

@property DelayedUITask *refreshTask;
@property BOOL applicationCursor;
@property (nonatomic) NSUInteger windowSizeRequestID;
@property (nonatomic) int lastWindowCols;
@property (nonatomic) int lastWindowRows;

@property NSNumber *terminalsKey;
@property NSUUID *uuid;

@end

@interface CustomWebView : WKWebView
@end
@implementation CustomWebView
- (BOOL)becomeFirstResponder {
    if (@available(iOS 13.4, *)) {
        return [super becomeFirstResponder];
    }
    return NO;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(copy:) || action == @selector(paste:)) {
        return NO;
    }
    return [super canPerformAction:action withSender:sender];
}
@end

static NSString *LRTerminalBootstrapBackground(NSString *hex) {
    id setting = [NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerTerminal.trueBlack"];
    if (setting && ![setting boolValue]) return hex;
    UIColor *c = [[UIColor alloc] ish_initWithHexString:hex]; CGFloat r = 1, g = 1, b = 1;
    if ([c getRed:&r green:&g blue:&b alpha:NULL] && (r + g + b) / 3 < 0.3) return @"#000000";
    return hex;
}
@implementation Terminal
@synthesize webView = _webView;

static const int BUF_SIZE = 1<<14;

static NSMapTable<NSNumber *, Terminal *> *terminals;
static NSMapTable<NSUUID *, Terminal *> *terminalsByUUID;

- (instancetype)initWithType:(int)type number:(int)num {
    @synchronized (Terminal.class) {
        self.terminalsKey = @(dev_make(type, num));
        Terminal *terminal = [terminals objectForKey:self.terminalsKey];
        if (terminal)
            return terminal;

        if (self = [super init]) {
            self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
            self.refreshTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(refresh)];
#if !ISH_LINUX
            lock_init(&_dataLock);
            cond_init(&_dataConsumed);
#endif

            [terminals setObject:self forKey:self.terminalsKey];
            self.uuid = [NSUUID UUID];
            [terminalsByUUID setObject:self forKey:self.uuid];
        }
        return self;
    }
}

- (WKWebView *)webView {
    if (_webView == nil) {
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        NSString *bootstrapStyleJSON = [self bootstrapStyleJSONForNewWebView];
        NSString *bootstrapStyleScript = [NSString stringWithFormat:
            @"(() => {"
             "const style = %@;"
             "window.__terminalInitialStyle = style;"
             "const apply = () => {"
                "const root = document.documentElement;"
                "if (!root) return;"
                "if (style.backgroundColor) root.style.setProperty('--terminal-background', style.backgroundColor);"
                "if (style.foregroundColor) root.style.setProperty('--terminal-foreground', style.foregroundColor);"
             "};"
             "apply();"
             "document.addEventListener('DOMContentLoaded', apply, {once: true});"
            "})();", bootstrapStyleJSON];
        WKUserScript *bootstrapStyleUserScript = [[WKUserScript alloc] initWithSource:bootstrapStyleScript
                                                                        injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                                     forMainFrameOnly:YES];
        [config.userContentController addUserScript:bootstrapStyleUserScript];
        [config.userContentController addScriptMessageHandler:self name:@"load"];
        [config.userContentController addScriptMessageHandler:self name:@"log"];
        [config.userContentController addScriptMessageHandler:self name:@"sendInput"];
        [config.userContentController addScriptMessageHandler:self name:@"resize"];
        [config.userContentController addScriptMessageHandler:self name:@"propUpdate"];
        // Make the web view really big so that if a program tries to write to the terminal before it's displayed, the text probably won't wrap too badly.
        CGRect webviewSize = CGRectMake(0, 0, 10000, 10000);
        _webView = [[CustomWebView alloc] initWithFrame:webviewSize configuration:config];
        if (@available(macOS 13.3, iOS 16.4, tvOS 16.4, *))
            _webView.inspectable = YES;
        _webView.layer.drawsAsynchronously = YES;
        _webView.scrollView.scrollEnabled = NO;
#if USE_XTERM_RENDERER
        NSURL *xtermHtmlFile = [NSBundle.mainBundle URLForResource:@"xterm-term" withExtension:@"html"];
#else
        NSURL *xtermHtmlFile = [NSBundle.mainBundle URLForResource:@"term" withExtension:@"html"];
#endif
        // Give WebKit access to the containing bundle directory so the
        // terminal frontend can load adjacent classic scripts and assets.
        [_webView loadFileURL:xtermHtmlFile allowingReadAccessToURL:xtermHtmlFile.URLByDeletingLastPathComponent];
    }
    return _webView;
}

- (NSString *)bootstrapStyleJSONForNewWebView {
    if (self.bootstrapStyleJSON.length > 0)
        return self.bootstrapStyleJSON;

    UserPreferences *prefs = UserPreferences.shared;
    Palette *palette = prefs.palette;
    NSMutableDictionary<NSString *, id> *themeInfo = [@{
        @"fontFamily": prefs.fontFamily,
        @"fontSize": prefs.fontSize,
        @"foregroundColor": palette.foregroundColor,
        @"backgroundColor": LRTerminalBootstrapBackground(palette.backgroundColor),
        @"cursorColor": palette.cursorColor ?: palette.foregroundColor,
        @"blinkCursor": @(prefs.blinkCursor),
        @"cursorShape": prefs.htermCursorShape,
        @"sharpness": [NSUserDefaults.standardUserDefaults stringForKey:@"LinuxRunnerTerminal.sharpness"] ?: @"sharp",
    } mutableCopy];
    if (palette.colorPaletteOverrides)
        themeInfo[@"colorPaletteOverrides"] = palette.colorPaletteOverrides;

    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:themeInfo options:0 error:nil];
    NSString *json = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
    return json ?: @"{}";
}

#if !ISH_LINUX
+ (Terminal *)createPseudoTerminal:(struct tty **)tty {
    *tty = pty_open_fake(&ios_pty_driver);
    if (IS_ERR(*tty))
        return nil;
    return (__bridge Terminal *) (*tty)->data;
}
#endif

- (void)setTty:(tty_t)tty {
    @synchronized (self) {
        _tty = tty;
        _lastWindowCols = 0;
        _lastWindowRows = 0;
        _windowSizeRequestID++;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self syncWindowSize];
    });
}

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"load"]) {
        NSLog(@"terminal frontend loaded");
        self.loaded = YES;
        [self.refreshTask schedule];
        // make sure this setting works if it's set before loading
        self.enableVoiceOverAnnounce = self.enableVoiceOverAnnounce;
    } else if ([message.name isEqualToString:@"log"]) {
        NSLog(@"%@", message.body);
    } else if ([message.name isEqualToString:@"sendInput"]) {
        if (![message.body isKindOfClass:NSString.class])
            return;
        NSData *data = [message.body dataUsingEncoding:NSUTF8StringEncoding];
        if (data == nil)
            return;
        [self sendInput:data];
    } else if ([message.name isEqualToString:@"resize"]) {
        [self syncWindowSize];
    } else if ([message.name isEqualToString:@"propUpdate"]) {
        if (![message.body isKindOfClass:NSArray.class])
            return;
        NSArray *body = message.body;
        if (body.count != 2 || ![body[0] isKindOfClass:NSString.class])
            return;
        NSString *property = body[0];
        if ([property isEqualToString:@"applicationCursor"] && [body[1] isKindOfClass:NSNumber.class])
            self.applicationCursor = [body[1] boolValue];
    }
}

- (void)syncWindowSize {
    NSUInteger requestID;
    @synchronized (self) {
        requestID = ++_windowSizeRequestID;
    }
    [self.webView evaluateJavaScript:@"exports.getSize()" completionHandler:^(NSArray<NSNumber *> *dimensions, NSError *error) {
        if (error != nil || ![dimensions isKindOfClass:NSArray.class] || dimensions.count < 2 ||
            ![dimensions[0] isKindOfClass:NSNumber.class] || ![dimensions[1] isKindOfClass:NSNumber.class])
            return;
        int cols = dimensions[0].intValue;
        int rows = dimensions[1].intValue;
        tty_t tty;
        @synchronized (self) {
            if (requestID != self->_windowSizeRequestID)
                return;
            tty = self->_tty;
            if (cols <= 0 || rows <= 0 || tty == NULL)
                return;
            if (cols == self->_lastWindowCols && rows == self->_lastWindowRows)
                return;
            self->_lastWindowCols = cols;
            self->_lastWindowRows = rows;
        }
#if !ISH_LINUX
        lock(&tty->lock);
        tty_set_winsize(tty, (struct winsize_) {.col = cols, .row = rows});
        unlock(&tty->lock);
#else
        async_do_in_workqueue(^{
            @synchronized (self) {
                if (self->_tty != tty)
                    return;
                tty->ops->resize(tty, cols, rows);
            }
        });
#endif
    }];
}

- (void)setEnableVoiceOverAnnounce:(BOOL)enableVoiceOverAnnounce {
    _enableVoiceOverAnnounce = enableVoiceOverAnnounce;
    [self.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.setAccessibilityEnabled(%@)",
                                      enableVoiceOverAnnounce ? @"true" : @"false"]
                   completionHandler:nil];
}

- (int)sendOutput:(const void *)buf length:(int)len {
#if !ISH_LINUX
    lock(&_dataLock);
    if (!NSThread.isMainThread) {
        // The main thread is the only one that can unblock this, so sleeping here would be a deadlock.
        // The only reason for this to be called on the main thread is if input is echoed.
        while (_pendingData.length > BUF_SIZE)
            wait_for_ignore_signals(&_dataConsumed, &_dataLock, NULL);
    }
    [_pendingData appendData:[NSData dataWithBytes:buf length:len]];
    [self.refreshTask schedule];
    unlock(&_dataLock);
#else
    @synchronized (self) {
        int room = [self roomForOutput];
        if (len > room)
            len = room;
        if (len > 0) {
            [_pendingData appendData:[NSData dataWithBytes:buf length:len]];
            [_refreshTask schedule];
        }
    }
#endif
    return len;
}

#if ISH_LINUX
- (int)roomForOutput {
    @synchronized (self) {
        if (_pendingData.length > BUF_SIZE)
            return 0;
        return BUF_SIZE - (int) _pendingData.length;
    }
}
#endif

- (void)sendInput:(NSData *)input {
    tty_t tty;
    @synchronized (self) {
        tty = self->_tty;
    }
    if (tty == NULL || input == nil)
        return;
#if !ISH_LINUX
    tty_input(tty, input.bytes, input.length, 0);
#else
    async_do_in_workqueue(^{
        NSData *inputRef = input;
        @synchronized (self) {
            if (self->_tty != tty)
                return;
            tty->ops->send_input(tty, inputRef.bytes, inputRef.length);
        }
    });
#endif
}

- (NSString *)arrow:(char)direction {
    return [NSString stringWithFormat:@"\x1b%c%c", self.applicationCursor ? 'O' : '[', direction];
}

- (void)refresh {
    if (!self.loaded)
        return;

#if !ISH_LINUX
    lock(&_dataLock);
    if (_outputInProgress) {
        unlock(&_dataLock);
        return;
    }
    NSData *data = _pendingData;
    if (data.length == 0) {
        unlock(&_dataLock);
        return;
    }
    _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
    _outputInProgress = YES;
    notify(&self->_dataConsumed);
    unlock(&_dataLock);
#else
    NSData *data;
    @synchronized (self) {
        if (_outputInProgress)
            return;
        data = _pendingData;
        if (data.length == 0)
            return;
        _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
        _outputInProgress = YES;
        tty_t tty = self->_tty;
        if (tty != NULL)
            async_do_in_irq(^{
                @synchronized (self) {
                    if (self->_tty != tty)
                        return;
                    tty->ops->can_output(tty);
                }
            });
    }
#endif

    NSString *dataString = [[NSString alloc] initWithBytes:data.bytes length:data.length encoding:NSISOLatin1StringEncoding];
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:@[dataString ?: @""] options:0 error:nil];
    NSString *jsonArgs = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
    NSString *jsToEvaluate = [NSString stringWithFormat:@"exports.write.apply(null, %@)", jsonArgs ?: @"[\"\"]"];
    [self.webView evaluateJavaScript:jsToEvaluate completionHandler:^(id result, NSError *error) {
        BOOL needsRefresh = NO;
#if !ISH_LINUX
        lock(&self->_dataLock);
        self->_outputInProgress = NO;
        needsRefresh = self->_pendingData.length > 0;
        unlock(&self->_dataLock);
#else
        @synchronized (self) {
            self->_outputInProgress = NO;
            needsRefresh = self->_pendingData.length > 0;
        }
#endif
        if (needsRefresh)
            [self.refreshTask schedule];
        if (error != nil) {
            NSLog(@"error sending bytes to the terminal: %@", error);
            return;
        }
    }];
}

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize {
    char *p = argv;
    for (NSString *cmd in command) {
        const char *c = cmd.UTF8String;
        // Save space for the final NUL byte in argv
        while (p < argv + maxSize - 1 && (*p++ = *c++));
        // If we reach the end of the buffer, the last string still needs to be
        // NUL terminated
        *p = '\0';
    }
    // Add the final NUL byte to argv
    *++p = '\0';
}

+ (Terminal *)terminalWithType:(int)type number:(int)number {
    return [[Terminal alloc] initWithType:type number:number];
}

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid {
    @synchronized (Terminal.class) {
        return [terminalsByUUID objectForKey:uuid];
    }
}

- (BOOL)foregroundCommandRunning {
#if !ISH_LINUX
    // Two plain reads under the same lock tty cleanup uses to clear _tty (no
    // tty->lock: cleanup may hold it while taking this one).
    @synchronized (self) {
        tty_t tty = self->_tty;
        if (tty == NULL) return NO;
        pid_t_ fg = tty->fg_group, session = tty->session;
        return fg > 0 && session > 0 && fg != session;
    }
#else
    return NO;
#endif
}

- (void)destroy {
    tty_t tty = self.tty;
    if (tty != NULL) {
#if !ISH_LINUX
        if (tty != NULL) {
            lock(&tty->lock);
            tty_hangup(tty);
            unlock(&tty->lock);
        }
#else
        tty->ops->hangup(tty);
#endif
    }
    @synchronized (Terminal.class) {
        [terminals removeObjectForKey:self.terminalsKey];
    }
}

+ (void)initialize {
    if (self == Terminal.class) {
        terminals = [NSMapTable strongToWeakObjectsMapTable];
        terminalsByUUID = [NSMapTable strongToWeakObjectsMapTable];
    }
}

@end

#if ISH_LINUX
nsobj_t Terminal_terminalWithType_number(int type, int number) {
    return CFBridgingRetain([Terminal terminalWithType:type number:number]);
}
int Terminal_sendOutput_length(nsobj_t _self, const char *data, int size) {
    return [(__bridge Terminal *) _self sendOutput:data length:size];
}
int Terminal_roomForOutput(nsobj_t _self) {
    return [(__bridge Terminal *) _self roomForOutput];
}
void Terminal_setLinuxTTY(nsobj_t _self, struct linux_tty *tty) {
    return [(__bridge Terminal *) _self setTty:tty];
}
#endif

#if !ISH_LINUX
static int ios_tty_init(struct tty *tty) {
    // This is called with ttys_lock but that results in deadlock since the main thread can also acquire ttys_lock. So release it.
    unlock(&ttys_lock);
    void (^init_block)(void) = ^{
        Terminal *terminal = [Terminal terminalWithType:tty->type number:tty->num];
        tty->data = (void *) CFBridgingRetain(terminal);
        terminal.tty = tty;
    };
    if ([NSThread isMainThread])
        init_block();
    else
        dispatch_sync(dispatch_get_main_queue(), init_block);

    lock(&ttys_lock);
    return 0;
}

static int ios_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    Terminal *terminal = (__bridge Terminal *) tty->data;
    return [terminal sendOutput:buf length:(int) len];
}

static void ios_tty_cleanup(struct tty *tty) {
    Terminal *terminal = CFBridgingRelease(tty->data);
    tty->data = NULL;
    terminal.tty = NULL;
}

struct tty_driver_ops ios_tty_ops = {
    .init = ios_tty_init,
    .write = ios_tty_write,
    .cleanup = ios_tty_cleanup,
};
DEFINE_TTY_DRIVER(ios_console_driver, &ios_tty_ops, TTY_CONSOLE_MAJOR, 64);
struct tty_driver ios_pty_driver = {.ops = &ios_tty_ops};
#endif
