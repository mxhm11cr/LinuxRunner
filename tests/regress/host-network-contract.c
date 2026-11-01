#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"NETWORK_FAIL line=%d errno=%d %s\n",__LINE__,errno,strerror(errno));return 1;} }while(0)
static int endpoint(int family,int type,int port) {
 int fd=socket(family,type|SOCK_CLOEXEC,0);if(fd<0)return -1;
 struct sockaddr_storage addr={0};socklen_t n;
 if(family==AF_INET){struct sockaddr_in *a=(void*)&addr;a->sin_family=family;a->sin_port=htons(port);a->sin_addr.s_addr=htonl(INADDR_LOOPBACK);n=sizeof *a;}
 else{struct sockaddr_in6 *a=(void*)&addr;a->sin6_family=family;a->sin6_port=htons(port);a->sin6_addr=in6addr_loopback;n=sizeof *a;}
 if(fcntl(fd,F_SETFL,O_NONBLOCK)<0){close(fd);return -1;}
 int r=connect(fd,(void*)&addr,n);if(r<0&&errno!=EINPROGRESS){close(fd);return -1;}
 struct pollfd p={fd,POLLOUT,0};if(poll(&p,1,3000)!=1){close(fd);return -1;}
 int error=0;socklen_t len=sizeof error;if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len)||error){close(fd);errno=error;return -1;}
 fcntl(fd,F_SETFL,0);struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);return fd;
}
static int transfer(int family,int type,int port,unsigned iterations) {
 int fd=endpoint(family,type,port);CHECK(fd>=0);
 unsigned char out[8192],in[8192];for(unsigned i=0;i<sizeof out;i++)out[i]=(i*31)^0xa9;
 for(unsigned i=0;i<iterations;i++){
  size_t off=0;while(off<sizeof out){ssize_t n=send(fd,out+off,sizeof out-off,MSG_NOSIGNAL);CHECK(n>0);off+=n;}
  off=0;while(off<sizeof in){ssize_t n=recv(fd,in+off,sizeof in-off,0);CHECK(n>0);off+=n;}
  CHECK(!memcmp(out,in,sizeof in));
 }
 close(fd);return 0;
}
int main(int argc,char **argv) {
 CHECK(argc==6);int ports[5];for(int i=0;i<5;i++)ports[i]=atoi(argv[i+1]);
 struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
 CHECK(transfer(AF_INET,SOCK_STREAM,ports[0],128)==0);
 clock_gettime(CLOCK_MONOTONIC,&t1);
 printf("NETWORK_TCP_ECHO bytes=1048576 seconds=%.6f (controlled loopback; includes payload verification)\n",t1.tv_sec-t0.tv_sec+(t1.tv_nsec-t0.tv_nsec)/1e9);
 CHECK(transfer(AF_INET,SOCK_DGRAM,ports[1],4)==0);
 if(ports[2]){CHECK(transfer(AF_INET6,SOCK_STREAM,ports[2],16)==0);CHECK(transfer(AF_INET6,SOCK_DGRAM,ports[3],4)==0);}
 for(int i=0;i<24;i++){int fd=endpoint(AF_INET,SOCK_STREAM,ports[0]);CHECK(fd>=0);close(fd);}
 CHECK(endpoint(AF_INET,SOCK_STREAM,ports[4])==-1&&errno==ECONNREFUSED);
 struct ifaddrs *all;CHECK(getifaddrs(&all)==0);unsigned ips=0;
 for(struct ifaddrs *p=all;p;p=p->ifa_next)if(p->ifa_addr&&(p->ifa_addr->sa_family==AF_INET||p->ifa_addr->sa_family==AF_INET6)){
  CHECK(if_nametoindex(p->ifa_name)>0);ips++;
 }freeifaddrs(all);CHECK(ips>0);
 FILE *dev=fopen("/proc/net/dev","r");CHECK(dev);char line[512];unsigned interfaces=0;
 while(fgets(line,sizeof line,dev)){char *colon=strchr(line,':');if(!colon)continue;*colon=0;char name[IF_NAMESIZE];CHECK(sscanf(line,"%15s",name)==1);CHECK(if_nametoindex(name)>0);interfaces++;}fclose(dev);CHECK(interfaces>0);
 clock_gettime(CLOCK_MONOTONIC,&t1);double seconds=t1.tv_sec-t0.tv_sec+(t1.tv_nsec-t0.tv_nsec)/1e9;
 printf("NETWORK_CONTRACT_PASS IPv4/IPv6=%s TCP/UDP nonblocking connect/poll/SO_ERROR, refusal, interfaces=%u elapsed=%.6f\n",ports[2]?"both":"IPv4 only",interfaces,seconds);
 return 0;
}
