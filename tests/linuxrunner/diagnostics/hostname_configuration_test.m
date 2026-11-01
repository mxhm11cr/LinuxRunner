#import <Foundation/Foundation.h>
#import "app/LinuxRunner/HostNameConfiguration.h"
#include <assert.h>
int main(void) {@autoreleasepool {
 NSString *original=@"127.0.0.1 localhost linuxrunner\n::1 localhost\n192.0.2.2 saved-host # personal alias\n";
 NSString *a=LRHostsForName(original,@"iPhone");assert([a hasPrefix:original]&&[a containsString:@"127.0.1.1 iPhone"]);
 assert([LRHostsForName(a,@"iPhone") isEqual:a]);
 NSString *b=LRHostsForName(a,@"iPad");assert(![b containsString:@" iPhone\t"]&&[b containsString:@"127.0.1.1 iPad"]&&[b hasPrefix:original]);
 assert([LRHostsForName(original,@"saved-host") isEqual:original]);
 assert([LRHostsForName(original,@"LOCALHOST") isEqual:original]);
 assert(LRHostsForName(original,@"injected\n192.0.2.3 nope")==nil);
 assert(LRHostsForName(original,@"")==nil);
 assert([LRHostsForName(@"127.0.0.1 localhost",@"iPhone") containsString:@"localhost\n127.0.1.1"]);
 puts("HOSTNAME_CONFIGURATION_PASS own alias update, idempotence, existing mappings retained, injection refused");
}return 0;}
