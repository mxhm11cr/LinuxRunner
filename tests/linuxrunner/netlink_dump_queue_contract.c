#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
/* Each successful send must yield exactly one complete dump, even if the
 * reply queue fills between messages. No terminal message from a failed send. */
int main(void) {
 int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_NONBLOCK,NETLINK_ROUTE);assert(fd>=0);
 unsigned sent=0,done=0,seen[8000]={0};
 struct request {struct nlmsghdr h;struct ifinfomsg i;} r[8];
 for(unsigned j=0;j<8;j++)r[j]=(struct request){.h={sizeof(r[j]),RTM_GETLINK,NLM_F_REQUEST|NLM_F_DUMP,0,0}};
 for(;sent<1000;sent++){for(unsigned j=0;j<8;j++)r[j].h.nlmsg_seq=sent*8+j+1;int n=send(fd,r,sizeof r,0);if(n<0){assert(errno==EAGAIN);break;}assert(n==sizeof r);}
 assert(sent>0&&sent<1000);char b[4096];int n;
 while((n=recv(fd,b,sizeof b,0))>=0){
  for(struct nlmsghdr *h=(void*)b;NLMSG_OK(h,n);h=NLMSG_NEXT(h,n)){
   assert(h->nlmsg_seq>0&&h->nlmsg_seq<=sent*8);assert(h->nlmsg_type!=NLMSG_ERROR);
   if(h->nlmsg_type==NLMSG_DONE){assert(!seen[h->nlmsg_seq-1]);seen[h->nlmsg_seq-1]=1;done++;}
  }assert(n==0);
 }
 assert(errno==EAGAIN&&done==sent*8);struct pollfd p={fd,POLLIN,0};assert(poll(&p,1,0)==0);
 for(unsigned j=0;j<8;j++)r[j].h.nlmsg_seq=8001+j;assert(send(fd,r,sizeof r,0)==sizeof r);assert(poll(&p,1,1000)==1);
 close(fd);printf("NETLINK_QUEUE_PASS batches=%u complete_dumps=%u bounded-pressure/drain/refill\n",sent,done);return 0;
}
