// Native macOS parser contract, including hostile/truncated host message data.
#include <assert.h>
#include <stdio.h>
#include "fs/netlink.c"
int errno_map(void) { return -errno; }
static struct lr_route seen;
static int capture(const struct lr_route *r, void *ctx) { (*(unsigned *)ctx)++; seen=*r; return 0; }
int main(void) {
#ifdef __APPLE__
 struct {struct lr_rt_msghdr h; struct sockaddr_in dst, gateway, mask;} r={0};
 r.h=(struct lr_rt_msghdr){.msglen=sizeof r,.version=5,.type=4,.index=3,.flags=LR_RTF_UP|LR_RTF_GATEWAY,.addrs=7};
 r.dst=(struct sockaddr_in){.sin_len=sizeof r.dst,.sin_family=AF_INET};
 r.gateway=r.dst;r.gateway.sin_addr.s_addr=htonl(0xc0000201);r.mask=r.dst;
 unsigned count=0;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)==0&&count==1);
 assert(seen.dst_len==0&&seen.has_gateway&&seen.ifindex==3&&seen.type==1);
 assert(!memcmp(seen.gateway,&r.gateway.sin_addr,4));
 r.h.flags=LR_RTF_UP;r.dst.sin_addr.s_addr=htonl(0xc0000200);r.mask.sin_addr.s_addr=htonl(0xffffff00);
 count=0;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)==0&&count==1&&seen.dst_len==24);
 r.h.flags|=LR_RTF_IFSCOPE;count=0;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)==0&&count==0);r.h.flags=LR_RTF_UP;
 for(size_t n=1;n<sizeof r;n++)assert(route_parse((void*)&r,n,AF_INET,capture,&count)<0);
 r.h.version=99;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)<0);r.h.version=5;
 r.dst.sin_len=255;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)<0);r.dst.sin_len=sizeof r.dst;
 r.h.msglen=0;assert(route_parse((void*)&r,sizeof r,AF_INET,capture,&count)<0);
 // Deterministic parser length/version/address bitmap sweep under sanitizers.
 unsigned char bytes[512];
 for(unsigned i=0;i<20000;i++){
  memset(bytes,0,sizeof bytes);memcpy(bytes,&r,sizeof r);struct lr_rt_msghdr h=r.h;
  h.msglen=i%513;h.version=i%8;h.addrs=i%256;memcpy(bytes,&h,sizeof h);
  int result=route_parse((void*)bytes,i%513,AF_INET,capture,&count);assert(result==0||result<0);
 }
 count=0;assert(lr_route_snapshot(AF_INET,capture,&count)==0);
 printf("DARWIN_ROUTE_PASS parser/IPv4/default/filter/20000 malformed cases, real_routes=%u\n",count);
#else
 puts("DARWIN_ROUTE_SKIP not Apple");
#endif
}
