// Simulator-only end-to-end bridge test. Fresh owned Simulator; no provider or
// physical hardware claim. Native import -> standard-user guest edit -> export.
+ (void)runDesktopFilesContract:(void (^)(NSDictionary *))completion {
    NSMutableDictionary *result=[@{@"scope":@"iOS Simulator app sandbox and ARM64 guest; native bridge calls, not external Files provider UI",@"physical_device":@NO} mutableCopy];
    if(!self.isRunning){result[@"status"]=@"BLOCKED";result[@"reason"]=@"runtime not running";completion(result);return;}
    TerminalViewController *setup=[self terminalForCommand:@[@"/bin/sh",@"-ec",@"id lrshare >/dev/null 2>&1 || adduser -D -h /home/lrshare -s /bin/sh lrshare\nid lrpreserve >/dev/null 2>&1 || adduser -D -h /home/lrpreserve -s /bin/sh lrpreserve\nmkdir -p /home/lrpreserve/Shared\nprintf keep-existing > /home/lrpreserve/Shared/keep.txt\n"] directory:@"/"];
    LRWaitForProcess(setup,30,^(BOOL ended){
        NSError *error=nil;BOOL selected=ended&&[self setDefaultLinuxUser:@"lrshare" error:&error];result[@"standard_user_selected"]=@(selected);
        if(!selected){result[@"status"]=@"FAIL";result[@"reason"]=error.localizedDescription?:@"account setup failed";completion(result);return;}
        NSString *root=self.activeRootName,*name=@"résumé-مرحبا.txt";
        NSData *input=[@"native import\n" dataUsingEncoding:NSUTF8StringEncoding];
        NSURL *source=[NSFileManager.defaultManager.temporaryDirectory URLByAppendingPathComponent:name];[input writeToURL:source atomically:YES];
        LRTransferControl *control=[LRTransferControl new];
        BOOL imported=[self importSharedFile:source relative:@"" root:root control:control error:&error];result[@"native_import"]=@(imported);
        NSError *collision=nil;BOOL second=[self importSharedFile:source relative:@"" root:root control:[LRTransferControl new] error:&collision];result[@"collision_preserved"]=@(!second&&collision.code==EEXIST);
        [NSFileManager.defaultManager removeItemAtURL:source error:nil];
        NSURL *shared=[self sharedExchangeURL:root];
        [self deleteExport:[shared URLByAppendingPathComponent:name]];result[@"export_cleanup_preserves_shared_source"]=@([NSFileManager.defaultManager fileExistsAtPath:[shared URLByAppendingPathComponent:name].path]);
        symlink("/etc/passwd",[shared URLByAppendingPathComponent:@"escape"].fileSystemRepresentation);
        NSError *linkError=nil;NSURL *refused=[self exportSharedItem:@"escape" relative:@"" root:root control:[LRTransferControl new] error:&linkError];result[@"symlink_refused"]=@(refused==nil);
        struct fs_info *testFS=LRAccountFS();struct task testContext={.uid=1000,.euid=1000,.gid=1000,.egid=1000,.fs=testFS};struct task *previousTask=current;current=&testContext;
        result[@"vfs_mkdir_result"]=@(generic_mkdirat(AT_PWD,"/mnt/LinuxRunnerShared/vfs-probe",0755));current=previousTask;fs_info_release(testFS);
        TerminalViewController *guest=[self terminalForCommand:@[@"/bin/su",@"-l",@"lrshare",@"-s",@"/bin/sh",@"-c",@"exec 2>\"$HOME/Shared/transfer-errors.txt\"; set -eu; test \"$(id -u)\" != 0; test -w /home/mobile/Downloads; test -w \"$HOME/Shared\"; test \"$(cat \"$HOME/Shared/résumé-مرحبا.txt\")\" = 'native import'; printf 'guest edit\\n' >> \"$HOME/Shared/résumé-مرحبا.txt\"; mkdir -p \"$HOME/Shared/generated\"; printf 'guest created\\n' > \"$HOME/Shared/generated/output.txt\"; printf 'unicode\\n' > \"$HOME/Shared/generated/写真.txt\"\n"] directory:@"/"];
        LRWaitForProcess(guest,30,^(BOOL guestEnded){
            result[@"guest_transfer_errors"]=[NSString stringWithContentsOfURL:[shared URLByAppendingPathComponent:@"transfer-errors.txt"] encoding:NSUTF8StringEncoding error:nil]?:@"";
            NSError *exportError=nil;NSURL *export=[self exportSharedItem:name relative:@"" root:root control:[LRTransferControl new] error:&exportError];
            NSData *expected=[@"native import\nguest edit\n" dataUsingEncoding:NSUTF8StringEncoding];
            result[@"standard_user_edit_and_export"]=@(guestEnded&&[(export?[NSData dataWithContentsOfURL:export]:nil) isEqual:expected]);if(export)[self deleteExport:export];
            NSURL *generated=[self exportSharedItem:@"output.txt" relative:@"generated" root:root control:[LRTransferControl new] error:nil];result[@"guest_generated_export"]=@([(generated?[NSString stringWithContentsOfURL:generated encoding:NSUTF8StringEncoding error:nil]:nil) isEqual:@"guest created\n"]);if(generated)[self deleteExport:generated];
            NSURL *zip=[self exportSharedItem:@"generated" relative:@"" root:root control:[LRTransferControl new] error:nil];result[@"directory_archive"]=@(zip!=nil);if(zip)[self deleteExport:zip];
            // Test diagnostic privacy and the real GUI screen factories.
            NSURL *logs=[[self exchangeURL:root] URLByAppendingPathComponent:@".linuxrunner/gui/session-00000000-0000-0000-0000-000000000028"];
            [NSFileManager.defaultManager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:nil];
            [@"qt.qpa.xcb: fatal test password=NEVER_EXPORT_THIS\nprivate ordinary document text\n" writeToURL:[logs URLByAppendingPathComponent:@"plasma.log"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            [@"ERROR UNAUTHORIZED_SYMLINK\n" writeToURL:source atomically:YES encoding:NSUTF8StringEncoding error:nil];
            symlink(source.fileSystemRepresentation,[logs URLByAppendingPathComponent:@"xvfb.log"].fileSystemRepresentation);
            NSArray *records=[self desktopDiagnosticEntries];NSString *description=records.description;
            result[@"diagnostic_privacy"]=@([description containsString:@"fatal test"]&&![description containsString:@"NEVER_EXPORT_THIS"]&&![description containsString:@"private ordinary document text"]&&![description containsString:@"UNAUTHORIZED_SYMLINK"]);
            @autoreleasepool {LRGUIViewController *viewer=[[LRGUIViewController alloc]initDesktop];[viewer loadViewIfNeeded];UIViewController *settings=[LinuxRunnerShell makeDesktopSettingsWithController:viewer],*logScreen=[LinuxRunnerShell makeDesktopLogsController];[settings loadViewIfNeeded];[logScreen loadViewIfNeeded];result[@"settings_and_logs_load"]=@(settings.view!=nil&&logScreen.view!=nil);}
            [NSFileManager.defaultManager removeItemAtURL:source error:nil];
            [self setDefaultLinuxUser:@"lrpreserve" error:nil];
            result[@"existing_home_shared_preserved"]=@([[[NSString alloc]initWithData:LRReadGuestFile(@"/home/lrpreserve/Shared/keep.txt",64) encoding:NSUTF8StringEncoding] isEqual:@"keep-existing"]);
            [self setDefaultLinuxUser:@"lrshare" error:nil];
            BOOL pass=YES;for(NSString *key in @[@"standard_user_selected",@"native_import",@"collision_preserved",@"symlink_refused",@"standard_user_edit_and_export",@"guest_generated_export",@"directory_archive",@"diagnostic_privacy",@"settings_and_logs_load",@"export_cleanup_preserves_shared_source",@"existing_home_shared_preserved"])pass&=[result[key] boolValue];
            result[@"status"]=pass?@"PASS":@"FAIL";if(exportError)result[@"export_error"]=exportError.localizedDescription;completion(result);
        });
    });
}
