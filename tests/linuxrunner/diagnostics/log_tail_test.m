#import "app/LinuxRunner/DiagnosticLogTail.h"
#include <assert.h>
int main(void){@autoreleasepool{
 assert(LRReadDiagnosticTail(nil,64)==nil);
 assert(LRReadDiagnosticTail([NSURL URLWithString:@"https://example.invalid/log"],64)==nil);
 NSString *folder=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 assert([NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:NO attributes:nil error:nil]);
 NSString *path=[folder stringByAppendingPathComponent:@"large.log"];int fd=open(path.fileSystemRepresentation,O_CREAT|O_RDWR,0600);assert(fd>=0);
 assert(ftruncate(fd,1024LL*1024*1024)==0);assert(pwrite(fd,"TAIL",4,1024LL*1024*1024-4)==4);close(fd);
 NSData *tail=LRReadDiagnosticTail([NSURL fileURLWithPath:path],64);assert(tail.length==64&&!memcmp((const char*)tail.bytes+60,"TAIL",4));
 NSString *link=[folder stringByAppendingPathComponent:@"link"];assert(symlink(path.fileSystemRepresentation,link.fileSystemRepresentation)==0);
 assert(LRReadDiagnosticTail([NSURL fileURLWithPath:link],64)==nil);assert(LRReadDiagnosticTail([NSURL fileURLWithPath:folder],64)==nil);
 assert(LRReadDiagnosticTail([NSURL fileURLWithPath:path],0).length==0);
 assert([@"☃fatal" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
 assert([LRDiagnosticTailString(LRReadDiagnosticTail([NSURL fileURLWithPath:path],6)) isEqual:@"fatal"]);
 assert([NSFileManager.defaultManager removeItemAtPath:folder error:nil]);puts("DIAGNOSTIC_TAIL_PASS 1 GiB sparse log -> 64-byte bounded read, symlink/directory refusal");
}}
