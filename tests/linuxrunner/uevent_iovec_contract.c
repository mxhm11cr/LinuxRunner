// Linux snapshots iovec descriptors before a blocking recvmsg. A second thread
// may reuse/change the userspace vector while the original syscall is asleep.
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
struct receive_state {int fd;atomic_int entered;struct iovec vector[2];char first[8],second[8],changed[16];ssize_t result;};
static void *receive_message(void *arg){struct receive_state *s=arg;s->vector[0]=(struct iovec){s->first,4};s->vector[1]=(struct iovec){s->second,4};struct msghdr m={.msg_iov=s->vector,.msg_iovlen=2};atomic_store(&s->entered,1);s->result=recvmsg(s->fd,&m,0);return NULL;}
int main(void){assert(!getuid());alarm(5);int rx=socket(AF_NETLINK,SOCK_RAW,NETLINK_KOBJECT_UEVENT),tx=socket(AF_NETLINK,SOCK_RAW,NETLINK_KOBJECT_UEVENT);assert(rx>=0&&tx>=0);assert(!(fcntl(rx,F_GETFL)&O_NONBLOCK));assert(!(fcntl(tx,F_GETFL)&O_NONBLOCK));struct sockaddr_nl name={.nl_family=AF_NETLINK};assert(!bind(rx,(void *)&name,sizeof(name)));socklen_t length=sizeof(name);assert(!getsockname(rx,(void *)&name,&length));
 struct receive_state state={.fd=rx};pthread_t thread;assert(!pthread_create(&thread,NULL,receive_message,&state));while(!atomic_load(&state.entered))usleep(1000);usleep(50000);
 state.vector[0]=(struct iovec){state.changed,1};state.vector[1]=(struct iovec){state.changed+1,7};
 assert(sendto(tx,"ABCDEFGH",8,0,(void *)&name,sizeof(name))==8);assert(!pthread_join(thread,NULL));assert(state.result==8&&!memcmp(state.first,"ABCD",4)&&!memcmp(state.second,"EFGH",4));char zeros[16]={0};assert(!memcmp(state.changed,zeros,16));close(rx);close(tx);alarm(0);puts("UEVENT_IOVEC_SNAPSHOT_PASS blocked receive uses entry-time descriptors");return 0;}
