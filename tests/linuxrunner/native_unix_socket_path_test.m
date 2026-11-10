#import <Foundation/Foundation.h>
#import "app/UnixSocketDirectory.h"
#include "fs/unix_socket_path.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>

int main(void) { @autoreleasepool {
    char out[128];
    const uint32_t values[] = {0, 1, 9, 10, 100, 65535, INT32_MAX, UINT32_MAX};
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++)
        for (unsigned j=0;j<sizeof(values)/sizeof(values[0]);j++) {
            size_t n=unix_socket_host_path("ishsock",values[i],values[j],out,sizeof(out));
            assert(n && n==strlen(out) && n<=7+13);
            errno=0;char *end=NULL;
            uint64_t decoded=strtoull(out+7,&end,36);
            assert(!errno && !*end && decoded==(((uint64_t)values[i]<<32)|values[j]));
            char exact[128];memset(exact,'?',sizeof(exact));
            assert(unix_socket_host_path("ishsock",values[i],values[j],exact,n+1)==n);
            assert(exact[n]==0 && exact[n+1]=='?');
            memset(exact,'?',sizeof(exact));
            assert(!unix_socket_host_path("ishsock",values[i],values[j],exact,n));
            for(size_t k=0;k<sizeof(exact);k++)assert(exact[k]=='?');
        }
    assert(!unix_socket_host_path("",0,0,out,0));
    assert(!unix_socket_host_path("too-long",1,1,out,3));
    NSString *missing=@"/private/var/tmp/linuxrunner-unix-no-such-directory";
    assert([LRUnixSocketDirectory(missing) isEqual:missing]);
    NSMutableString *directory=[[NSString stringWithFormat:@"/private/var/tmp/lr-socket-%d-%@",getpid(),NSUUID.UUID.UUIDString] mutableCopy];
    while([directory lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<88)[directory appendString:@"x"];
    assert([directory lengthOfBytesUsingEncoding:NSUTF8StringEncoding]==88);
    NSError *error=nil;
    assert([NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:&error]);
    NSString *original=[directory stringByAppendingString:@"/"];
    NSString *shorter=LRUnixSocketDirectory(original);
    assert([shorter lengthOfBytesUsingEncoding:NSUTF8StringEncoding]==81);
    NSString *oldPrefix=[original stringByAppendingString:@"ishsock"];
    struct sockaddr_un address={.sun_family=AF_UNIX};
    assert(snprintf(address.sun_path,sizeof(address.sun_path),"%s%d.%u",oldPrefix.fileSystemRepresentation,27082,10)>=(int)sizeof(address.sun_path));
    NSString *prefix=[shorter stringByAppendingPathComponent:@"ishsock"];
    const uint32_t pairs[][2]={{27082,10},{INT32_MAX,UINT32_MAX},{UINT32_MAX,UINT32_MAX}};
    for(unsigned i=0;i<3;i++) {
        size_t n=unix_socket_host_path(prefix.fileSystemRepresentation,pairs[i][0],pairs[i][1],address.sun_path,sizeof(address.sun_path));
        assert(n && n<sizeof(address.sun_path));
        int fd=socket(AF_UNIX,SOCK_DGRAM,0);assert(fd>=0);
        assert(bind(fd,(struct sockaddr *)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+n))==0);
        struct timeval timeout={.tv_sec=1};
        assert(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0);
        int client=socket(AF_UNIX,SOCK_DGRAM,0);assert(client>=0);
        assert(sendto(client,"ok",2,0,(struct sockaddr *)&address,(socklen_t)(offsetof(struct sockaddr_un,sun_path)+n))==2);
        char message[2];assert(recv(fd,message,sizeof(message),0)==2 && !memcmp(message,"ok",2));
        close(client);
        close(fd);assert(unlink(address.sun_path)==0);
    }
    assert([NSFileManager.defaultManager removeItemAtPath:directory error:&error]);
    puts("NATIVE_UNIX_SOCKET_PATH_PASS ID roundtrip, bounds, verified alias and real Darwin bind/send/receive");
} return 0; }
