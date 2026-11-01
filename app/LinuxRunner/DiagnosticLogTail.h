#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
// Fixed diagnostic filenames only. Bound IO/allocation before reading; a
// multi-gigabyte guest log must not be loaded into the iOS process to take a tail.
static NSData *LRReadDiagnosticTail(NSURL *url,NSUInteger limit) {
    if (!url.isFileURL) return nil;
    int fd=open(url.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return nil;
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0){close(fd);return nil;}
    size_t bytes=(uint64_t)st.st_size>limit?limit:(size_t)st.st_size;
    NSMutableData *data=[NSMutableData dataWithLength:bytes];ssize_t n;
    do{n=pread(fd,data.mutableBytes,bytes,st.st_size-bytes);}while(n<0&&errno==EINTR);
    close(fd);if(n<0)return nil;data.length=n;return data;
}

static NSString *LRDiagnosticTailString(NSData *data) {
    if(!data)return nil;
    // A byte-bounded tail may start inside one UTF-8 codepoint.
    for(NSUInteger skip=0;skip<4&&skip<=data.length;skip++) {
        NSData *part=skip?[data subdataWithRange:NSMakeRange(skip,data.length-skip)]:data;
        NSString *text=[[NSString alloc]initWithData:part encoding:NSUTF8StringEncoding];
        if(text)return text;
    }
    return nil;
}
