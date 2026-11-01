#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static atomic_int race_fd=-1;
static void *close_race(void *unused) {
 (void)unused;sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
 for(unsigned i=0;i<3000;i++) {int fd=signalfd(-1,&mask,SFD_NONBLOCK);assert(fd>=0);atomic_store(&race_fd,fd);sched_yield();close(fd);atomic_store(&race_fd,-1);}
 return NULL;
}
int main(void) {
 sigset_t mask,old;sigemptyset(&mask);sigaddset(&mask,SIGUSR1);sigaddset(&mask,SIGUSR2);assert(!sigprocmask(SIG_BLOCK,&mask,&old));
 int fd=signalfd(-1,&mask,SFD_NONBLOCK|SFD_CLOEXEC),ep=epoll_create1(EPOLL_CLOEXEC);assert(fd>=0&&ep>=0);
 struct epoll_event event={.events=EPOLLIN|EPOLLET,.data.fd=fd};assert(!epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event));
 struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,0)==0);
 for(unsigned i=0;i<3;i++){
  assert(!kill(getpid(),SIGUSR1));assert(poll(&p,1,0)==1&&(p.revents&POLLIN));
  assert(epoll_wait(ep,&event,1,0)==1&&(event.events&EPOLLIN));
  struct signalfd_siginfo info;assert(read(fd,&info,sizeof info)==sizeof info&&info.ssi_signo==SIGUSR1);
  assert(poll(&p,1,0)==0&&epoll_wait(ep,&event,1,0)==0);
 }
 pid_t child=fork();assert(child>=0);if(!child){usleep(40000);assert(!kill(getppid(),SIGUSR2));_exit(0);}
 struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);assert(poll(&p,1,2000)==1);clock_gettime(CLOCK_MONOTONIC,&b);
 struct signalfd_siginfo info;assert(read(fd,&info,sizeof info)==sizeof info&&info.ssi_signo==SIGUSR2&&info.ssi_pid==(unsigned)child);
 int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 sigemptyset(&mask);sigaddset(&mask,SIGUSR1);assert(signalfd(fd,&mask,0)==fd);
 assert(!kill(getpid(),SIGUSR2));assert(poll(&p,1,0)==0);
 sigaddset(&mask,SIGUSR2);assert(signalfd(fd,&mask,0)==fd);assert(poll(&p,1,0)==1);
 assert(read(fd,&info,sizeof info)==sizeof info&&info.ssi_signo==SIGUSR2);
 close(fd);close(ep);
 pthread_t thread;assert(!pthread_create(&thread,NULL,close_race,NULL));
 for(unsigned i=0;i<12000;i++) {int target=atomic_load(&race_fd);if(target<0)continue;int result=signalfd(target,&mask,0);assert(result==target||(result==-1&&errno==EBADF));}
 assert(!pthread_join(thread,NULL));assert(!sigprocmask(SIG_SETMASK,&old,0));
 printf("SIGNALFD_POLL_PASS poll/epoll-ET/mask update/child signal elapsed_ms=%.3f\n",(b.tv_sec-a.tv_sec)*1000+(b.tv_nsec-a.tv_nsec)/1e6);return 0;
}
