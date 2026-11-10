//
//  ViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

@class TerminalView;
@interface TerminalViewController : UIViewController
/// The terminal view (font size, clipboard) for LinuxRunner's terminal screen.
@property (weak, nonatomic, readonly) TerminalView *termView;

@property (nonatomic) Terminal *terminal;
// Optional per-session argv for the LinuxRunner launcher. The normal shell
// continues to use the upstream preferences and automatic restart behavior.
@property (nonatomic, copy) NSArray<NSString *> *runnerCommand;
@property (nonatomic, copy) NSString *runnerDirectory;
@property BOOL runnerReportsExit;

- (void)startNewSession;
/// Types text into this terminal's session (as if from the keyboard).
- (void)sendSessionInput:(NSString *)text;
- (void)reconnectSessionFromTerminalUUID:(NSUUID *)uuid;
@property (readonly) NSUUID *sessionTerminalUUID; // 0 means invalid
@property UISceneSession *sceneSession API_AVAILABLE(ios(13.0));

@end

extern struct tty_driver ios_tty_driver;
