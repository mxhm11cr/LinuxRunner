#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"MREMAP_MEMFD_FAIL line=%d errno=%d %s\n",__LINE__,errno,strerror(errno));return 1;}}while(0)
static int test(int move,int read_only) {
 const size_t page=4096,offset=4096,total=8*4096;
 int fd=memfd_create("wayland-pool-regression",MFD_CLOEXEC|MFD_ALLOW_SEALING);CHECK(fd>=0&&ftruncate(fd,offset+total)==0);
 unsigned char data[4096];memset(data,'G',sizeof data);CHECK(pwrite(fd,data,sizeof data,offset+page)==sizeof data);
 unsigned char *space=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(space!=MAP_FAILED);
 int mapfd=fd;
 if(read_only){char path[80];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);mapfd=open(path,O_RDONLY);CHECK(mapfd>=0);}
 unsigned char *p=mmap(space,page,read_only?PROT_READ:PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,mapfd,offset);CHECK(p==space);
 unsigned char *alias=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_SHARED,fd,offset);CHECK(alias!=MAP_FAILED);alias[0]='A';
 if(!move)CHECK(munmap(space+page,3*page)==0);
 close(fd);if(read_only)close(mapfd); // mapping must retain the live backing identity.
 if(move){CHECK(mremap(p,page,4*page,0)==MAP_FAILED&&errno==ENOMEM);CHECK(p[0]=='A');}
 unsigned char *q=mremap(p,page,4*page,move?MREMAP_MAYMOVE:0);CHECK(q!=MAP_FAILED);CHECK(move?q!=p:q==p);
 CHECK(q[0]=='A'&&q[page]=='G');alias[0]='B';CHECK(q[0]=='B');
 if(read_only)CHECK(mprotect(q,4*page,PROT_READ|PROT_WRITE)==-1&&errno==EACCES);
 else{q[page]='R';CHECK(alias[page]=='R');}
 CHECK(mremap(q,4*page,2*page,0)==q);CHECK(q[0]=='B');
 CHECK(munmap(q,2*page)==0&&munmap(alias,total)==0);
 if(move)CHECK(munmap(space+page,total-page)==0);else CHECK(munmap(space+4*page,total-4*page)==0);
 return 0;
}
int main(void){alarm(20);for(int move=0;move<2;move++)for(int ro=0;ro<2;ro++)CHECK(test(move,ro)==0);puts("MREMAP_SHARED_MEMFD_PASS in-place/move, offset, closed fd, shared aliases, read-only permissions, shrink");return 0;}
