// Pure hosts-file policy shared by the app boot path and native contracts.
#import <Foundation/Foundation.h>
static inline NSString *LRHostsForName(NSString *original, NSString *name) {
    if (!original || !name.length || name.length > 253 ||
        [name rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_."] invertedSet]].location != NSNotFound)
        return nil;
    NSString *marker=@"\t# LinuxRunner host alias";
    NSMutableArray<NSString *> *lines=[NSMutableArray new];
    BOOL supplied=NO;
    for (NSString *line in [original componentsSeparatedByString:@"\n"]) {
        if ([line hasSuffix:marker]) continue; // Replace only our own old alias.
        [lines addObject:line];
        NSString *data=[[line componentsSeparatedByString:@"#"] firstObject];
        NSArray *words=[data componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        BOOL address=NO;
        for (NSString *word in words) if (word.length) {
            if (!address) { address=YES; continue; }
            if ([word caseInsensitiveCompare:name]==NSOrderedSame) supplied=YES;
        }
    }
    NSMutableString *updated=[[lines componentsJoinedByString:@"\n"] mutableCopy];
    if (!supplied) {
        if (updated.length && ![updated hasSuffix:@"\n"]) [updated appendString:@"\n"];
        [updated appendFormat:@"127.0.1.1 %@%@\n",name,marker];
    }
    return updated;
}
