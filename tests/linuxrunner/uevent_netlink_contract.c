#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static void option(int fd,int level,int key,int value){assert(!setsockopt(fd,level,key,&value,sizeof(value)));}
static uint32_t port(int fd){struct sockaddr_nl a; socklen_t n=sizeof(a);assert(!getsockname(fd,(void *)&a,&n)&&n==sizeof(a)&&a.nl_family==AF_NETLINK);return a.nl_pid;}
static int endpoint(unsigned groups){int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,NETLINK_KOBJECT_UEVENT);assert(fd>=0);assert(!port(fd));struct sockaddr_nl a={.nl_family=AF_NETLINK,.nl_groups=groups};assert(!bind(fd,(void *)&a,sizeof(a)));assert(port(fd));return fd;}
static int ready(int fd){struct pollfd p={fd,POLLIN,0};int n=poll(&p,1,0);assert(n>=0);return n>0&&!!(p.revents&POLLIN);}
static ssize_t send_to(int fd,uint32_t target,const void *p,size_t n,int flags){struct sockaddr_nl a={.nl_family=AF_NETLINK,.nl_pid=target};return sendto(fd,p,n,flags,(void *)&a,sizeof(a));}
struct reader {int fd;atomic_int waiting,done;ssize_t n;int error;char data[32];};
static void *read_thread(void *arg){struct reader *r=arg;struct iovec v={r->data,sizeof(r->data)};struct msghdr m={.msg_iov=&v,.msg_iovlen=1};atomic_store(&r->waiting,1);r->n=recvmsg(r->fd,&m,0);r->error=errno;atomic_store(&r->done,1);return NULL;}
static void wait_reader(struct reader *r){while(!atomic_load(&r->waiting))usleep(1000);usleep(50000);assert(!atomic_load(&r->done));}
struct writer {int fd;uint32_t target;atomic_int waiting,done;ssize_t n;int error;char bytes[512];};
static void *write_thread(void *arg){struct writer *w=arg;atomic_store(&w->waiting,1);w->n=send_to(w->fd,w->target,w->bytes,sizeof(w->bytes),0);w->error=errno;atomic_store(&w->done,1);return NULL;}
static volatile sig_atomic_t signaled;
static void signal_handler(int n){(void)n;signaled=1;}
int main(void){
 assert(getuid()==0);alarm(25);
 int receiver=endpoint(0),sender=endpoint(0);uint32_t rx=port(receiver),tx=port(sender);assert(rx!=tx);
 int collision=socket(AF_NETLINK,SOCK_DGRAM|SOCK_CLOEXEC,NETLINK_KOBJECT_UEVENT);assert(collision>=0);struct sockaddr_nl a={.nl_family=AF_NETLINK,.nl_pid=rx};assert(bind(collision,(void *)&a,sizeof(a))<0&&errno==EADDRINUSE);close(collision);
 int pass=1;assert(setsockopt(receiver,SOL_SOCKET,SO_TIMESTAMP,&pass,sizeof(pass))<0&&errno==EOPNOTSUPP);option(receiver,SOL_SOCKET,SO_PASSCRED,pass);option(receiver,SOL_NETLINK,NETLINK_PKTINFO,1);
 option(receiver,SOL_NETLINK,NETLINK_ADD_MEMBERSHIP,2);a=(struct sockaddr_nl){0};socklen_t length=sizeof(a);assert(!getsockname(receiver,(void *)&a,&length)&&a.nl_groups==2);
 uint32_t membership=0;length=sizeof(membership);assert(!getsockopt(receiver,SOL_NETLINK,NETLINK_LIST_MEMBERSHIPS,&membership,&length)&&membership==2&&length==4);
 char first[]="abc",second[]="def";struct iovec parts[]={{first,3},{second,3}};a=(struct sockaddr_nl){.nl_family=AF_NETLINK,.nl_pid=rx};struct msghdr message={.msg_name=&a,.msg_namelen=sizeof(a),.msg_iov=parts,.msg_iovlen=2};assert(sendmsg(sender,&message,0)==6&&ready(receiver));
 char buffer[8192];char control[CMSG_SPACE(sizeof(struct ucred))+CMSG_SPACE(sizeof(struct nl_pktinfo))];struct iovec shortpart={buffer,3};struct sockaddr_nl source;
 message=(struct msghdr){.msg_name=&source,.msg_namelen=sizeof(source),.msg_iov=&shortpart,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
 assert(recvmsg(receiver,&message,MSG_PEEK|MSG_TRUNC|MSG_CMSG_CLOEXEC)==6);assert((message.msg_flags&MSG_TRUNC)&&!(message.msg_flags&MSG_CTRUNC)&&!memcmp(buffer,"abc",3));assert(source.nl_pid==tx&&source.nl_groups==0);
 int credentials=0,packetinfo=0;for(struct cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)){
  if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_CREDENTIALS){struct ucred u;memcpy(&u,CMSG_DATA(c),sizeof(u));assert(u.pid==getpid()&&u.uid==getuid()&&u.gid==getgid());credentials++;}
  if(c->cmsg_level==SOL_NETLINK&&c->cmsg_type==NETLINK_PKTINFO){struct nl_pktinfo i;memcpy(&i,CMSG_DATA(c),sizeof(i));assert(i.group==0);packetinfo++;}
 }assert(credentials==1&&packetinfo==1);
 assert(recv(receiver,buffer,2,0)==2&&!memcmp(buffer,"ab",2));assert(recv(receiver,buffer,sizeof(buffer),0)<0&&errno==EAGAIN&&!ready(receiver));
 assert(send_to(sender,rx,NULL,0,0)<0&&errno==ENODATA&&!ready(receiver));
 for(unsigned i=0;i<sizeof(buffer);i++)buffer[i]=(i*17)%251;
 assert(send_to(sender,rx,buffer,sizeof(buffer),0)==sizeof(buffer));int queued=0;assert(!ioctl(receiver,FIONREAD,&queued)&&queued==sizeof(buffer));char tiny[2]={0};assert(recv(receiver,tiny,sizeof(tiny),MSG_TRUNC|MSG_PEEK)==sizeof(buffer)&&tiny[1]==17);assert(ready(receiver));assert(recv(receiver,tiny,sizeof(tiny),MSG_TRUNC)==sizeof(buffer)&&!ready(receiver));
 struct sock_filter instructions[]={BPF_STMT(BPF_LD|BPF_B|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,'a',0,1),BPF_STMT(BPF_RET|BPF_K,2),BPF_STMT(BPF_RET|BPF_K,0)};
 struct sock_fprog filter={.len=4,.filter=instructions};assert(!setsockopt(receiver,SOL_SOCKET,SO_ATTACH_FILTER,&filter,sizeof(filter)));
 length=0;assert(!getsockopt(receiver,SOL_SOCKET,SO_GET_FILTER,NULL,&length)&&length==4);struct sock_filter returned[4];length=4;assert(!getsockopt(receiver,SOL_SOCKET,SO_GET_FILTER,returned,&length)&&!memcmp(returned,instructions,sizeof(returned)));
 assert(send_to(sender,rx,"xyz",3,0)==3&&!ready(receiver));assert(send_to(sender,rx,"abcdef",6,0)==6);assert(recv(receiver,buffer,sizeof(buffer),0)==2&&!memcmp(buffer,"ab",2));
 struct sock_filter bad[]={BPF_STMT(BPF_JMP|BPF_JA,100),BPF_STMT(BPF_RET|BPF_K,0)};filter=(struct sock_fprog){2,bad};assert(setsockopt(receiver,SOL_SOCKET,SO_ATTACH_FILTER,&filter,sizeof(filter))<0&&errno==EINVAL);
 bad[0]=(struct sock_filter)BPF_STMT(BPF_LD|BPF_MEM,0);assert(setsockopt(receiver,SOL_SOCKET,SO_ATTACH_FILTER,&filter,sizeof(filter))<0&&errno==EINVAL);
 option(receiver,SOL_SOCKET,SO_LOCK_FILTER,1);assert(setsockopt(receiver,SOL_SOCKET,SO_ATTACH_FILTER,&filter,sizeof(filter))<0);int zero=0;assert(setsockopt(receiver,SOL_SOCKET,SO_DETACH_FILTER,&zero,sizeof(zero))<0&&errno==EPERM);assert(setsockopt(receiver,SOL_SOCKET,SO_LOCK_FILTER,&zero,sizeof(zero))<0&&errno==EPERM);close(receiver);
 receiver=endpoint(2);rx=port(receiver);option(receiver,SOL_SOCKET,SO_PASSCRED,1);int other=endpoint(2);struct sockaddr_nl group={.nl_family=AF_NETLINK,.nl_groups=2};ssize_t n=sendto(sender,"real guest message",18,0,(void *)&group,sizeof(group));assert(n<0&&errno==ECONNREFUSED);assert(recv(receiver,buffer,sizeof(buffer),0)==18&&!memcmp(buffer,"real guest message",18));assert(recv(other,buffer,sizeof(buffer),0)==18);option(other,SOL_NETLINK,NETLINK_DROP_MEMBERSHIP,2);
 n=sendto(sender,"second",6,0,(void *)&group,sizeof(group));assert(n<0&&errno==ECONNREFUSED);assert(recv(receiver,buffer,sizeof(buffer),0)==6);assert(!ready(other));close(other);
 pid_t child=fork();assert(child>=0);if(!child){assert(!setgid(65534)&&!setuid(65534));int listen=endpoint(2);assert(send_to(listen,rx,"forged",6,0)<0&&errno==EPERM);_exit(0);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
 option(receiver,SOL_SOCKET,SO_RCVBUF,8192);unsigned filled=0;
 while(send_to(sender,rx,buffer,512,MSG_DONTWAIT)>=0){filled++;assert(filled<1000);}assert(filled>2&&errno==EAGAIN);
 option(receiver,SOL_SOCKET,SO_RCVBUF,1024);assert(send_to(sender,rx,buffer,512,MSG_DONTWAIT)<0&&errno==EAGAIN);option(receiver,SOL_SOCKET,SO_RCVBUF,8192);
 assert(!fcntl(sender,F_SETFL,fcntl(sender,F_GETFL)&~O_NONBLOCK));struct writer w={.fd=sender,.target=rx};pthread_t writer;assert(!pthread_create(&writer,NULL,write_thread,&w));while(!atomic_load(&w.waiting))usleep(1000);usleep(50000);assert(!atomic_load(&w.done));
 assert(recv(receiver,buffer,sizeof(buffer),0)==512);assert(!pthread_join(writer,NULL)&&w.n==512);unsigned drained=0;while(recv(receiver,buffer,sizeof(buffer),0)>=0)drained++;assert(errno==EAGAIN&&drained==filled&&!ready(receiver));
 assert(!fcntl(receiver,F_SETFL,fcntl(receiver,F_GETFL)&~O_NONBLOCK));struct timeval timeout={0,50000};assert(!setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));struct timespec began,ended;clock_gettime(CLOCK_MONOTONIC,&began);assert(recv(receiver,buffer,sizeof(buffer),0)<0&&errno==EAGAIN);clock_gettime(CLOCK_MONOTONIC,&ended);long elapsed=(ended.tv_sec-began.tv_sec)*1000+(ended.tv_nsec-began.tv_nsec)/1000000;assert(elapsed>=20&&elapsed<3000);
 timeout=(struct timeval){0};assert(!setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));struct sigaction action={.sa_handler=signal_handler};sigemptyset(&action.sa_mask);assert(!sigaction(SIGUSR1,&action,NULL));struct reader r={.fd=receiver};pthread_t thread;assert(!pthread_create(&thread,NULL,read_thread,&r));wait_reader(&r);assert(!pthread_kill(thread,SIGUSR1)&&!pthread_join(thread,NULL));assert(r.n<0&&r.error==EINTR&&signaled);
 r=(struct reader){.fd=receiver};assert(!pthread_create(&thread,NULL,read_thread,&r));wait_reader(&r);int old=receiver;close(receiver);int replacement=open("/dev/null",O_RDONLY);assert(replacement==old);assert(send_to(sender,rx,"held",4,0)==4);assert(!pthread_join(thread,NULL)&&r.n==4&&!memcmp(r.data,"held",4));assert(send_to(sender,rx,"gone",4,MSG_DONTWAIT)<0&&errno==ECONNREFUSED);close(replacement);close(sender);
 alarm(0);puts("UEVENT_NETLINK_PASS unicast multicast membership credentials pktinfo filters peek truncation >4KiB zero-refusal timeout signal close-reuse permissions queue-pressure shrink-refusal blocking-writer");return 0;
}
