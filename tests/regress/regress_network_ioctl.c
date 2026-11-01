#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"NETWORK_IOCTL_FAIL line=%d errno=%d\n",__LINE__,errno);return 1;}}while(0)
int main(void){
 int fd=socket(AF_INET,SOCK_DGRAM,0);CHECK(fd>=0);
 struct ifreq data[32],q={0};struct ifconf conf={0};CHECK(ioctl(fd,SIOCGIFCONF,&conf)==0&&conf.ifc_len>0);
 int required=conf.ifc_len;conf.ifc_buf=(char*)data;conf.ifc_len=1;CHECK(ioctl(fd,SIOCGIFCONF,&conf)==0&&conf.ifc_len==0);
 conf.ifc_len=-1;CHECK(ioctl(fd,SIOCGIFCONF,&conf)==-1&&errno==EINVAL);
 conf.ifc_buf=(char*)1;conf.ifc_len=required;CHECK(ioctl(fd,SIOCGIFCONF,&conf)==-1&&errno==EFAULT);
 conf.ifc_req=data;conf.ifc_len=sizeof data;CHECK(ioctl(fd,SIOCGIFCONF,&conf)==0&&conf.ifc_len>0);
 for(int i=0;i<conf.ifc_len/(int)sizeof *data;i++){
  snprintf(q.ifr_name,sizeof q.ifr_name,"%s",data[i].ifr_name);
  CHECK(ioctl(fd,SIOCGIFADDR,&q)==0&&q.ifr_addr.sa_family==AF_INET);
  CHECK(ioctl(fd,SIOCGIFNETMASK,&q)==0&&q.ifr_netmask.sa_family==AF_INET);
  CHECK(ioctl(fd,SIOCGIFINDEX,&q)==0&&q.ifr_ifindex>0);
  CHECK(ioctl(fd,SIOCGIFNAME,&q)==0&&!strcmp(q.ifr_name,data[i].ifr_name));
  CHECK(ioctl(fd,SIOCGIFFLAGS,&q)==0);CHECK(ioctl(fd,SIOCGIFMTU,&q)==0&&q.ifr_mtu>0);
  struct ethtool_value link={.cmd=ETHTOOL_GLINK};q.ifr_data=(void *)&link;
  CHECK(ioctl(fd,SIOCETHTOOL,&q)==0&&link.data<=1);
  struct ethtool_ts_info ts={.cmd=ETHTOOL_GET_TS_INFO};q.ifr_data=(void *)&ts;
  CHECK(ioctl(fd,SIOCETHTOOL,&q)==-1&&errno==EOPNOTSUPP);
  q.ifr_data=(void *)1;CHECK(ioctl(fd,SIOCETHTOOL,&q)==-1&&errno==EFAULT);
 }
 snprintf(q.ifr_name,sizeof q.ifr_name,"lr_missing_if");CHECK(ioctl(fd,SIOCGIFINDEX,&q)==-1&&errno==ENODEV);
 // This view refuses setters before invoking any host ioctl.
 CHECK(ioctl(fd,SIOCSIFFLAGS,&q)==-1&&errno==ENOTTY);close(fd);
 puts("NETWORK_IOCTL_PASS IPv4 layout/index/name/MTU/flags, short buffers/EFAULT, setters refused");return 0;
}
