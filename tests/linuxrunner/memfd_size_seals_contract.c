#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_SEAL_SEAL 1
#define F_SEAL_SHRINK 2
#define F_SEAL_GROW 4
#endif
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"MEMFD_SEALS_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define ERROR(x,e) do{errno=0;CHECK((x)==-1&&errno==(e));}while(0)
static void *resize(void *arg){int fd=*(int*)arg;for(int i=0;i<500;i++)if(ftruncate(fd,8192)!=-1||errno!=EPERM)return (void*)1;return NULL;}
static void sendfd(int s,int fd){char byte='F',control[CMSG_SPACE(sizeof fd)]={0};struct iovec i={&byte,1};struct msghdr m={.msg_iov=&i,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};struct cmsghdr*c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof fd);memcpy(CMSG_DATA(c),&fd,sizeof fd);CHECK(sendmsg(s,&m,0)==1);}
static int recvfd(int s){char byte,control[CMSG_SPACE(sizeof(int))];struct iovec i={&byte,1};struct msghdr m={.msg_iov=&i,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof control};CHECK(recvmsg(s,&m,0)==1);struct cmsghdr*c=CMSG_FIRSTHDR(&m);CHECK(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS);int fd;memcpy(&fd,CMSG_DATA(c),sizeof fd);return fd;}
int main(void){alarm(20);int d=memfd_create("default",0);CHECK(d>=0&&fcntl(d,F_GET_SEALS)==F_SEAL_SEAL);ERROR(fcntl(d,F_ADD_SEALS,F_SEAL_GROW),EPERM);close(d);
 int f=memfd_create("resizable",MFD_CLOEXEC|MFD_ALLOW_SEALING);CHECK(f>=0&&fcntl(f,F_GET_SEALS)==0&&fcntl(f,F_GETFD)==FD_CLOEXEC);CHECK(ftruncate(f,8193)==0);CHECK(fcntl(f,F_ADD_SEALS,F_SEAL_GROW)==0&&fcntl(f,F_GET_SEALS)==F_SEAL_GROW);int copy=dup(f);CHECK(copy>=0&&fcntl(copy,F_GET_SEALS)==F_SEAL_GROW);
 ERROR(ftruncate(copy,8194),EPERM);CHECK(ftruncate(copy,8192)==0&&ftruncate(copy,8193)==-1&&errno==EPERM);CHECK(ftruncate(copy,8192)==0);
 // Add shrink after restoring a final partial page in a separately sized memfd.
 close(f);close(copy);f=memfd_create("chromium",MFD_ALLOW_SEALING);CHECK(f>=0&&ftruncate(f,8193)==0);CHECK(fcntl(f,F_ADD_SEALS,F_SEAL_GROW|F_SEAL_SHRINK)==0);
 char buf[10000];memset(buf,'G',sizeof buf);CHECK(pwrite(f,buf,sizeof buf,0)==8192);CHECK(pwrite(f,"P",1,8192)==1);ERROR(pwrite(f,"xx",2,8192),EPERM);ERROR(pwrite(f,"x",1,8193),EPERM);
 CHECK(lseek(f,8192,SEEK_SET)==8192);ERROR(write(f,"xx",2),EPERM);CHECK(write(f,"P",1)==1);CHECK(write(f,"",0)==0);
 CHECK(fcntl(f,F_SETFL,O_APPEND)==0&&lseek(f,0,SEEK_SET)==0);ERROR(write(f,"A",1),EPERM);ERROR(pwrite(f,"A",1,0),EPERM);CHECK(fcntl(f,F_SETFL,0)==0);
 ERROR(ftruncate(f,8192),EPERM);ERROR(ftruncate(f,8194),EPERM);CHECK(ftruncate(f,8193)==0);ERROR(syscall(SYS_fallocate,f,0,8193,1),EPERM);
 unsigned char *map=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_SHARED,f,0);CHECK(map!=MAP_FAILED);map[8192]='M';CHECK(pread(f,buf,1,8192)==1&&buf[0]=='M');CHECK(mprotect(map,12288,PROT_READ)==0&&mprotect(map,12288,PROT_READ|PROT_WRITE)==0);
 pthread_t t;CHECK(pthread_create(&t,NULL,resize,&f)==0);void *r;CHECK(pthread_join(t,&r)==0&&r==NULL);
 int pair[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);pid_t p=fork();CHECK(p>=0);if(!p){close(pair[0]);int received=recvfd(pair[1]);CHECK(fcntl(received,F_GET_SEALS)==6);CHECK(fcntl(received,F_ADD_SEALS,F_SEAL_SEAL)==0);ERROR(ftruncate(received,0),EPERM);CHECK(pwrite(received,"C",1,0)==1);close(received);_exit(0);}close(pair[1]);sendfd(pair[0],f);int status;CHECK(waitpid(p,&status,0)==p&&WIFEXITED(status)&&WEXITSTATUS(status)==0);CHECK(fcntl(f,F_GET_SEALS)==7&&map[0]=='C');ERROR(fcntl(f,F_ADD_SEALS,0),EPERM);
 int u=memfd_create("unknown-seal",MFD_ALLOW_SEALING);CHECK(u>=0);ERROR(fcntl(u,F_ADD_SEALS,0x80000002),EINVAL);CHECK(fcntl(u,F_GET_SEALS)==0);CHECK(fcntl(u,F_ADD_SEALS,8)==0);CHECK(fcntl(u,F_GET_SEALS)==8);ERROR(write(u,"x",1),EPERM);close(u);int regular=open("/dev/null",O_RDONLY);CHECK(regular>=0);ERROR(fcntl(regular,F_GET_SEALS),EINVAL);ERROR(fcntl(-1,F_GET_SEALS),EBADF);close(regular);
 struct stat st;CHECK(fstat(f,&st)==0&&st.st_size==8193);CHECK(munmap(map,12288)==0);close(pair[0]);close(f);printf("MEMFD_SIZE_SEALS_PASS checks=%u resize write pwrite append shared_map dup fork SCM_RIGHTS lifetime\n",checks);return 0;}
