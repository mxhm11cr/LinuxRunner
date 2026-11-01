// Installed libudev interoperability. A controlled guest-root producer sends
// userspace-format events; these are test messages, never captured host events.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <libudev.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <sys/wait.h>
#include <unistd.h>
struct event_header {char prefix[8];uint32_t magic,header_size,properties_off,properties_len,subsystem,devtype,bloom_hi,bloom_lo;};
struct ready_message {uint32_t port,subsystem_hash;};
static void exact_read(int fd,void *p,size_t n){assert(read(fd,p,n)==(ssize_t)n);}
static void exact_write(int fd,const void *p,size_t n){assert(write(fd,p,n)==(ssize_t)n);}
static void publish(int fd,uint32_t hash,const char *properties,size_t n){
 struct event_header h={.prefix="libudev",.magic=htonl(0xfeedcafe),.header_size=sizeof(h),.properties_off=sizeof(h),.properties_len=n,.subsystem=htonl(hash)};
 struct iovec v[]={{&h,sizeof(h)},{(void *)properties,n}};struct sockaddr_nl dst={.nl_family=AF_NETLINK,.nl_groups=2};struct msghdr m={.msg_name=&dst,.msg_namelen=sizeof(dst),.msg_iov=v,.msg_iovlen=2};
 assert(sendmsg(fd,&m,0)<0&&errno==ECONNREFUSED); // multicast delivered; no kernel unicast endpoint
}
int main(void){
 assert(getuid()==0);alarm(15);int sync[2],ack[2];assert(!pipe(sync)&&!pipe(ack));pid_t child=fork();assert(child>=0);
 if(!child){
  close(sync[1]);close(ack[0]);assert(!setgid(65534)&&!setuid(65534));struct udev *u=udev_new();assert(u);udev_set_log_priority(u,LOG_DEBUG);struct udev_monitor *m=udev_monitor_new_from_netlink(u,"udev");assert(m);int fd=udev_monitor_get_fd(m);assert(fd>=0);
  // With no guest daemon libudev may choose group NONE. Explicitly subscribe
  // this controlled test to its userspace group; do not create a fake service.
  int group=2;assert(!setsockopt(fd,SOL_NETLINK,NETLINK_ADD_MEMBERSHIP,&group,sizeof(group)));
  assert(!udev_monitor_filter_add_match_subsystem_devtype(m,"mem",NULL));assert(!udev_monitor_enable_receiving(m));
  struct sockaddr_nl name;socklen_t len=sizeof(name);assert(!getsockname(fd,(void *)&name,&len));
  struct sock_filter filter[512];len=512;assert(!getsockopt(fd,SOL_SOCKET,SO_GET_FILTER,filter,&len));assert(len>0&&len<=512);uint32_t hash=0;
  for(unsigned i=0;i+1<len;i++)if(filter[i].code==(BPF_LD|BPF_W|BPF_ABS)&&filter[i].k==offsetof(struct event_header,subsystem)&&filter[i+1].code==(BPF_JMP|BPF_JEQ|BPF_K))hash=filter[i+1].k;
  assert(hash);struct ready_message ready={name.nl_pid,hash};exact_write(ack[1],&ready,sizeof(ready));char token;exact_read(sync[0],&token,1);
  struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,0)==0);exact_write(ack[1],"F",1);
  assert(poll(&p,1,2000)==1);errno=0;struct udev_device *invalid=udev_monitor_receive_device(m);assert(!invalid&&errno==EINVAL);exact_write(ack[1],"M",1);
  assert(poll(&p,1,2000)==1);struct udev_device *device=udev_monitor_receive_device(m);if(!device)fprintf(stderr,"LIBUDEV_RECEIVE_FAILED errno=%d (%s)\n",errno,strerror(errno));assert(device);
  assert(!strcmp(udev_device_get_subsystem(device),"mem")&&!strcmp(udev_device_get_action(device),"change")&&!strcmp(udev_device_get_devnode(device),"/dev/null"));
  udev_device_unref(device);udev_monitor_unref(m);udev_unref(u);puts("LIBUDEV_EVENT_PASS nonroot monitor real guest-root credentials filter/drop parsed-event");fflush(stdout);_exit(0);
 }
 close(sync[0]);close(ack[1]);struct ready_message ready;exact_read(ack[0],&ready,sizeof(ready));assert(ready.port);
 int sender=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_KOBJECT_UEVENT);assert(sender>=0);
 const char wrong[]="ACTION=change\0DEVPATH=/devices/virtual/mem/null\0SUBSYSTEM=block\0DEVNAME=/dev/null\0MAJOR=1\0MINOR=3\0SEQNUM=1\0";
 publish(sender,0,wrong,sizeof(wrong)-1);exact_write(sync[1],"C",1);char token;exact_read(ack[0],&token,1);assert(token=='F');
 const char good[]="ACTION=change\0DEVPATH=/devices/virtual/mem/null\0SUBSYSTEM=mem\0DEVNAME=/dev/null\0MAJOR=1\0MINOR=3\0SEQNUM=2\0";
 // First deliberately include the extra C terminator; libudev must reject
 // the empty property. The valid event ends at the final explicit NUL.
 publish(sender,ready.subsystem_hash,good,sizeof(good));exact_read(ack[0],&token,1);assert(token=='M');
 publish(sender,ready.subsystem_hash,good,sizeof(good)-1);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));close(sender);close(sync[1]);close(ack[0]);alarm(0);return 0;
}
