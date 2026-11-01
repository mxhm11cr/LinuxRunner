//
//  SceneDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/26/19.
//

#import "SceneDelegate.h"
#import "AboutViewController.h"
#ifdef LINUXRUNNER
#import "LinuxRunner-Swift.h"
#import "LinuxRunner/RuntimeBridge.h"
#import "LinuxRunner/GUIViewController.h"
#endif

TerminalViewController *currentTerminalViewController = NULL;

@interface SceneDelegate ()

@property NSString *terminalUUID;
#ifdef LINUXRUNNER
@property TerminalViewController *runnerTerminal;
#endif

@end

static NSString *const TerminalUUID = @"TerminalUUID";

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    #ifdef LINUXRUNNER
    if (NO) { // LinuxRunner has no inherited recovery settings screen
#else
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
#endif
        UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
        AboutViewController *avc = (AboutViewController *) vc.topViewController;
        avc.recoveryMode = YES;
        self.window.rootViewController = vc;
        return;
    }

    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
    vc.sceneSession = session;
#ifdef LINUXRUNNER
    // The native workspace is usable before boot. Do not create a guest PTY
    // until initialization succeeds; retain this terminal for a manual start.
    if (!LRRuntimeBridge.isRunning) {
        self.runnerTerminal = vc;
        self.window.rootViewController = [LinuxRunnerShell makeControllerWithTerminal:vc];
        [LRGUIViewController registerExternalDisplayForController:self.window.rootViewController];
        return;
    }
#endif
    if (session.stateRestorationActivity == nil) {
        [vc startNewSession];
    } else {
        self.terminalUUID = session.stateRestorationActivity.userInfo[TerminalUUID];
        [vc reconnectSessionFromTerminalUUID:
         [[NSUUID alloc] initWithUUIDString:self.terminalUUID]];
    }
#ifdef LINUXRUNNER
    self.runnerTerminal = vc;
    self.window.rootViewController = [LinuxRunnerShell makeControllerWithTerminal:vc];
    [LRGUIViewController registerExternalDisplayForController:self.window.rootViewController];
#endif
}

- (NSUserActivity *)stateRestorationActivityForScene:(UIScene *)scene {
    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:@"dev.linuxrunner.scene"];
    TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
#ifdef LINUXRUNNER
    vc = self.runnerTerminal;
#endif
    if ([vc isKindOfClass:TerminalViewController.class]) {
        self.terminalUUID = vc.sessionTerminalUUID.UUIDString;
        if (self.terminalUUID != nil) {
            [activity addUserInfoEntriesFromDictionary:@{TerminalUUID: self.terminalUUID}];
        }
    }
    return activity;
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = (TerminalViewController *) self.window.rootViewController;;
#ifdef LINUXRUNNER
    terminalViewController = self.runnerTerminal;
    [LRRuntimeBridge logCategory:@"UI" event:@"foreground"];
#endif
    currentTerminalViewController = terminalViewController;
}

- (void)sceneWillResignActive:(UIScene *)scene {
    TerminalViewController *terminalViewController = (TerminalViewController *) self.window.rootViewController;

#ifdef LINUXRUNNER
    terminalViewController = self.runnerTerminal;
    [LRRuntimeBridge logCategory:@"UI" event:@"inactive/background"];
#endif
    if (currentTerminalViewController == terminalViewController) {
        currentTerminalViewController = NULL;
    }
}

@end
