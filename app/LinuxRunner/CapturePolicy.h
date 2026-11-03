#import <Foundation/Foundation.h>

// An explicit global choice keeps its historical meaning, including NO.
// Catalog identity is authoritative; installation metadata is the fallback
// only when this root has no catalog entry. Never infer SteamOS from Arch's
// package manager or a library name. Reading a default must not save it.
static inline BOOL LRDesktopSharedCaptureEnabled(NSUserDefaults *defaults,
        NSString *root, NSArray<NSDictionary *> *environments, NSDictionary *installation) {
    id choice = [defaults objectForKey:@"LinuxRunnerGUISharedImages"];
    if (choice) return [defaults boolForKey:@"LinuxRunnerGUISharedImages"];
    id distribution = installation[@"distributionID"];
    for (NSDictionary *environment in environments) {
        if ([environment[@"rootfsName"] isEqual:root]) {
            distribution = environment[@"distributionID"];
            break;
        }
    }
    return [distribution isEqual:@"arch"] || [distribution isEqual:@"steamos"];
}
