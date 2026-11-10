// Simulator-only deletion tests with disposable app-owned directories.
+ (void)runEnvironmentRemovalContract:(void (^)(NSDictionary *))completion {
    NSMutableDictionary *result=[@{@"scope":@"Simulator sandbox; disposable environments",@"physical_device":@NO} mutableCopy];
    if(!bootedRoot){result[@"status"]=@"FAIL";result[@"reason"]=@"Boot a disposable Simulator guest first";completion(result);return;}
    NSFileManager *files=NSFileManager.defaultManager;Roots *roots=Roots.instance;
    NSString *name=[@"remove-test-" stringByAppendingString:NSUUID.UUID.UUIDString.lowercaseString],*previous=roots.defaultRoot;
    NSURL *root=[self rootURL:name],*exchange=[self exchangeURL:name],*shared=[self sharedExchangeURL:name];
    NSURL *outside=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
    for(NSURL *url in @[root,exchange,shared,outside])[files createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil];
    NSData *keep=[@"keep external files" dataUsingEncoding:NSUTF8StringEncoding];
    NSURL *sentinel=[outside URLByAppendingPathComponent:@"keep.txt"];[keep writeToURL:sentinel atomically:YES];
    [keep writeToURL:[root URLByAppendingPathComponent:@"linux.txt"] atomically:YES];
    [keep writeToURL:[shared URLByAppendingPathComponent:@"shared.txt"] atomically:YES];
    [files createSymbolicLinkAtURL:[root URLByAppendingPathComponent:@"external"] withDestinationURL:outside error:nil];
    [[roots mutableOrderedSetValueForKey:@"roots"] addObject:name];
    NSURL *failure=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [files createDirectoryAtURL:failure withIntermediateDirectories:YES attributes:nil error:nil];
    [keep writeToURL:[failure URLByAppendingPathComponent:@"1"] atomically:YES];
    NSError *stageError=nil;BOOL staged=[roots stageRemovalOfRoot:name extraDirectories:@[exchange] destination:failure error:&stageError];
    result[@"failed_stage_rolls_back"]=@(!staged&&stageError&&[files fileExistsAtPath:[root URLByAppendingPathComponent:@"linux.txt"].path]&&[roots.roots containsObject:name]);
    [files removeItemAtURL:failure error:nil];
    __block BOOL activeProtected=NO,invalidProtected=NO;
    [self removeEnvironment:bootedRoot completion:^(NSError *error){activeProtected=error.code==EBUSY;}];
    [self removeEnvironment:@"../escape" completion:^(NSError *error){invalidProtected=error.code==EINVAL;}];
    result[@"active_root_protected"]=@(activeProtected);result[@"invalid_root_protected"]=@(invalidProtected);
    roots.defaultRoot=name;
    [NSUserDefaults.standardUserDefaults setObject:@"temporary-user" forKey:[@"LinuxRunnerDefaultUser." stringByAppendingString:name]];
    [self removeEnvironment:name completion:^(NSError *error){
        result[@"removed"]=@(!error&&![roots.roots containsObject:name]&&![files fileExistsAtPath:root.path]);
        result[@"owned_shared_files_removed"]=@(![files fileExistsAtPath:shared.path]&&![files fileExistsAtPath:exchange.path]);
        result[@"external_files_preserved"]=@([[NSData dataWithContentsOfURL:sentinel] isEqual:keep]);
        result[@"next_root_valid"]=@(![roots.defaultRoot isEqual:name]&&[roots.roots containsObject:roots.defaultRoot]);
        result[@"preferences_removed"]=@([NSUserDefaults.standardUserDefaults objectForKey:[@"LinuxRunnerDefaultUser." stringByAppendingString:name]]==nil);
        BOOL pass=YES;for(NSString *key in @[@"failed_stage_rolls_back",@"active_root_protected",@"invalid_root_protected",@"removed",@"owned_shared_files_removed",@"external_files_preserved",@"next_root_valid",@"preferences_removed"])pass&=[result[key] boolValue];
        result[@"status"]=pass?@"PASS":@"FAIL";
        roots.defaultRoot=previous;[files removeItemAtURL:outside error:nil];completion(result);
    }];
}
