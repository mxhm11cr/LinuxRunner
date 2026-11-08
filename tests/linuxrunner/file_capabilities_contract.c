#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/auxv.h>
#include <sys/wait.h>
#include <linux/capability.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FILECAP_FAIL line=%d %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
#define ERROR(x,e) do { errno=0; CHECK((x)==-1 && errno==(e)); } while (0)
static const char *name="security.capability";
static uint32_t caps[5]={0x02000001,1u<<CAP_NET_BIND_SERVICE,0,0,0};
static uint64_t getcaps(int permitted) {
 struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct d[2];
 CHECK(syscall(SYS_capget,&h,d)==0);return permitted?d[0].permitted|((uint64_t)d[1].permitted<<32):d[0].effective|((uint64_t)d[1].effective<<32);
}
static void wait_ok(pid_t p){int s;CHECK(waitpid(p,&s,0)==p&&WIFEXITED(s)&&!WEXITSTATUS(s));}
static void copy(const char *from,const char *to){char b[65536];int r=open(from,O_RDONLY),w=open(to,O_WRONLY|O_CREAT|O_EXCL,0755);CHECK(r>=0&&w>=0);ssize_t n;while((n=read(r,b,sizeof b))>0)CHECK(write(w,b,n)==n);CHECK(n==0);CHECK(!close(r)&&!close(w));}
static void set(const char *p){CHECK(setxattr(p,name,caps,sizeof caps,0)==0);}
static void absent(const char *p){ERROR(getxattr(p,name,NULL,0),ENODATA);}
static void child_exec(const char *p,const char *mode,int nnp,int drop){pid_t c=fork();CHECK(c>=0);if(!c){if(drop)CHECK(prctl(PR_CAPBSET_DROP,CAP_NET_BIND_SERVICE,0L,0L,0L)==0);CHECK(setgid(1001)==0&&setuid(1001)==0);CHECK(getcaps(0)==0&&getcaps(1)==0);if(nnp)CHECK(prctl(PR_SET_NO_NEW_PRIVS,1L,0L,0L,0L)==0);CHECK(prctl(PR_SET_PDEATHSIG,SIGUSR1)==0);execl(p,p,mode,NULL);if(drop&&errno==EPERM)_exit(0);perror("exec");_exit(2);}wait_ok(c);}
int main(int argc,char **argv){
 if(argc>1){
  CHECK(geteuid()==1001);
  uint64_t bit=1ull<<CAP_NET_BIND_SERVICE;
  if(!strcmp(argv[1],"--relay")){CHECK(getcaps(0)==bit);const char *target=getenv("LR_FILECAP_PLAIN");CHECK(target);execl(target,target,"--no-caps",NULL);CHECK(0);}
  else if(!strcmp(argv[1],"--granted")){
   CHECK(getcaps(0)==bit&&getcaps(1)==bit);CHECK(getauxval(AT_SECURE)==1);CHECK(prctl(PR_GET_DUMPABLE)==0);
   int sig=-1;CHECK(prctl(PR_GET_PDEATHSIG,&sig)==0&&sig==0);ERROR(prctl(PR_CAPBSET_DROP,CAP_NET_BIND_SERVICE,0L,0L,0L),EPERM);
   struct __user_cap_header_struct h={_LINUX_CAPABILITY_VERSION_3,0};struct __user_cap_data_struct d[2]={{0}};
   CHECK(!syscall(SYS_capset,&h,d));d[0].permitted=bit;ERROR(syscall(SYS_capset,&h,d),EPERM);
  }else if(!strcmp(argv[1],"--no-caps")){CHECK(!getcaps(0)&&!getcaps(1));CHECK(getauxval(AT_SECURE)==0);}
  else if(!strcmp(argv[1],"--permitted")){CHECK(getcaps(0)==0&&getcaps(1)==bit);CHECK(getauxval(AT_SECURE)==1);}
  else CHECK(0);
  return 0;
 }
 CHECK(geteuid()==0);
 char dir[160],p[200],q[200],ln[200];strcpy(dir,"/tmp/lr-filecaps-XXXXXX");CHECK(mkdtemp(dir)!=NULL);CHECK(!chmod(dir,0755));snprintf(p,sizeof p,"%s/program",dir);snprintf(q,sizeof q,"%s/renamed",dir);snprintf(ln,sizeof ln,"%s/link",dir);copy("/proc/self/exe",p);
 absent(p);ERROR(setxattr(p,name,caps,sizeof caps,XATTR_REPLACE),ENODATA);
 CHECK(!setxattr(p,name,caps,sizeof caps,XATTR_CREATE));ERROR(setxattr(p,name,caps,sizeof caps,XATTR_CREATE),EEXIST);
 CHECK(getxattr(p,name,NULL,0)==20);char b[128];ERROR(getxattr(p,name,b,19),ERANGE);CHECK(getxattr(p,name,b,sizeof b)==20&&!memcmp(b,caps,20));
 CHECK(listxattr(p,NULL,0)==20);ERROR(listxattr(p,b,19),ERANGE);CHECK(listxattr(p,b,sizeof b)==20&&!strcmp(b,name));
 ERROR(setxattr(p,name,caps,19,0),EINVAL);ERROR(setxattr(p,name,caps,20,3),EINVAL);
 uint32_t rev3[6]={0x03000001,1u<<CAP_NET_BIND_SERVICE,0,0,0,1001};ERROR(setxattr(p,name,rev3,sizeof rev3,0),EOPNOTSUPP);rev3[5]=0;CHECK(!setxattr(p,name,rev3,sizeof rev3,0));CHECK(getxattr(p,name,b,sizeof b)==20);
 int fd=open(p,O_RDONLY);CHECK(fd>=0);CHECK(!link(p,ln)&&!rename(p,q));CHECK(fgetxattr(fd,name,b,sizeof b)==20);CHECK(!removexattr(ln,name));absent(q);set(q);CHECK(!unlink(ln));CHECK(fgetxattr(fd,name,b,sizeof b)==20);
 int op=open(q,O_PATH);CHECK(op>=0);ERROR(fgetxattr(op,name,b,sizeof b),EBADF);close(op);
 CHECK(!symlink(q,ln));ERROR(lgetxattr(ln,name,b,sizeof b),ENODATA);CHECK(getxattr(ln,name,b,sizeof b)==20);CHECK(!unlink(ln));
 pid_t c=fork();CHECK(c>=0);if(!c){CHECK(!setgid(1001)&&!setuid(1001));ERROR(setxattr(q,name,caps,sizeof caps,0),EPERM);ERROR(removexattr(q,name),EPERM);_exit(0);}wait_ok(c);
 copy(q,p);CHECK(!setenv("LR_FILECAP_PLAIN",p,1));child_exec(q,"--relay",0,0);
 child_exec(q,"--granted",0,0);child_exec(q,"--no-caps",1,0);child_exec(q,"--granted",0,1);
 caps[0]=0x02000000;set(q);child_exec(q,"--permitted",0,0);caps[0]=0x02000001;
 // A regular exec must not retain a non-root file capability grant.
 CHECK(!removexattr(q,name));child_exec(q,"--no-caps",0,0);
 set(q);ERROR(write(fd,"x",1),EBADF);CHECK(fgetxattr(fd,name,b,sizeof b)==20);
 c=fork();CHECK(c>=0);if(!c){CHECK(!setgid(1001)&&!setuid(1001));ERROR(open(q,O_WRONLY|O_TRUNC),EACCES);_exit(0);}wait_ok(c);CHECK(getxattr(q,name,b,sizeof b)==20);
 set(q);int w=open(q,O_RDWR);CHECK(w>=0);CHECK(pwrite(w,"\177",1,0)==1);absent(q);set(q);CHECK(lseek(w,0,SEEK_SET)==0&&write(w,"\177",1)==1);absent(q);
 set(q);CHECK(!ftruncate(w,lseek(w,0,SEEK_END)));absent(q);set(q);CHECK(!chown(q,0,0));absent(q);set(q);CHECK(!fchown(w,0,0));absent(q);
 set(q);void *map=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,w,0);CHECK(map!=MAP_FAILED);absent(q);CHECK(!munmap(map,4096));close(w);
 set(q);CHECK(!unlink(q));CHECK(fgetxattr(fd,name,b,sizeof b)==20);CHECK(!fremovexattr(fd,name));ERROR(fgetxattr(fd,name,b,sizeof b),ENODATA);close(fd);CHECK(!unlink(p));CHECK(!rmdir(dir));
 puts("FILE_CAPABILITIES_CONTRACT_PASS storage flags links fds exec secure NNP bounding credentials mutation");return 0;
}
