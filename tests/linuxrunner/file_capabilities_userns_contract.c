#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <linux/capability.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){perror(#x);fprintf(stderr,"FILECAP_USERNS_FAIL line=%d\n",__LINE__);exit(1);}}while(0)
static uint64_t caps(void){struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct d[2];CHECK(!syscall(SYS_capget,&h,d));return d[0].effective|((uint64_t)d[1].effective<<32);}
static void child(int bounded){pid_t p=fork();CHECK(p>=0);if(!p){CHECK(!unshare(CLONE_NEWUSER));int fd=open("/proc/self/uid_map",O_WRONLY);CHECK(fd>=0&&write(fd,"0 0 1\n",6)==6);close(fd);CHECK(geteuid()==0&&caps());
 if(bounded){CHECK(!prctl(PR_CAPBSET_DROP,CAP_NET_BIND_SERVICE,0L,0L,0L));
  struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct d[2];CHECK(!syscall(SYS_capget,&h,d));d[0].inheritable|=1u<<CAP_NET_BIND_SERVICE;errno=0;CHECK(syscall(SYS_capset,&h,d)==-1&&errno==EPERM);
 }
 else {struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct d[2]={{0}};CHECK(!syscall(SYS_capset,&h,d));CHECK(!prctl(PR_SET_NO_NEW_PRIVS,1L,0L,0L,0L));}
 execl("/proc/self/exe","filecaps-userns",bounded?"--bounded":"--nnp",NULL);CHECK(0);
 }int s;CHECK(waitpid(p,&s,0)==p&&WIFEXITED(s)&&!WEXITSTATUS(s));}
int main(int argc,char **argv){if(argc>1){CHECK(geteuid()==0);uint64_t e=caps();if(!strcmp(argv[1],"--bounded")){CHECK(e&&!(e&(1ull<<CAP_NET_BIND_SERVICE)));CHECK(prctl(PR_CAPBSET_READ,CAP_NET_BIND_SERVICE,0L,0L,0L)==0);}else{CHECK(!e);CHECK(prctl(PR_GET_NO_NEW_PRIVS,0L,0L,0L,0L)==1);}return 0;}child(1);child(0);puts("FILE_CAPABILITIES_USERNS_PASS bounding and no_new_privs survive namespace-root exec");return 0;}
