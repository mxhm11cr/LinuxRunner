// Guest NETLINK_KOBJECT_UEVENT transport. This is an in-runtime datagram bus,
// not access to Darwin kernel uevents or physical USB devices. Without a guest
// udev service libudev may intentionally subscribe to no groups, just as in a
// Linux container. Actual userspace messages still carry real sender identity.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/real.h"
#include "fs/poll.h"
#include "fs/uevent.h"
#include "fs/socket_filter.h"

extern const struct fd_ops socket_fdops;
#define UEVENT_MAX_MESSAGE (64*1024)
#define UEVENT_MAX_QUEUE (4*1024*1024)
#define UEVENT_ALL_QUEUES (16*1024*1024)
#define SOL_NETLINK_ 270
struct uevent_packet {
    struct uevent_packet *next;
    size_t size;
    struct sockaddr_nl_ sender;
    struct ucred_ credentials;
    uint32_t group;
    uint8_t bytes[];
};
struct uevent_socket {
    struct list link;
    uint32_t port,groups;
    uint64_t generation;
    bool bound,connected,passcred,pktinfo,locked,no_enobufs,broadcast_error;
    struct sockaddr_nl_ destination;
    struct uevent_packet *head,*tail;
    size_t queued,rcvbuf,sndbuf;
    struct timespec rcvtimeo,sndtimeo;
    int error;
    struct lr_bpf_insn *filter;
    size_t filter_len;
    struct fd *fd;
};
static lock_t uevent_lock=LOCK_INITIALIZER;
static cond_t uevent_changed=COND_INITIALIZER;
static struct list uevent_sockets=LIST_INITIALIZER(uevent_sockets);
static uint32_t next_port=UINT32_MAX;
static uint64_t next_generation;
static size_t all_queued;
#ifdef GUEST_ARM64
#define uevent_msghdr msghdr64_
#define uevent_iovec iovec64_
#define uevent_cmsghdr cmsghdr64_
struct uevent_fprog {uint16_t count;uint16_t pad[3];uint64_t pointer;};
#else
#define uevent_msghdr msghdr_
#define uevent_iovec iovec_
#define uevent_cmsghdr cmsghdr_
struct uevent_fprog {uint16_t count;uint16_t pad;uint32_t pointer;};
#endif
static struct uevent_socket *lookup(uint32_t port) {
    struct uevent_socket *s;list_for_each_entry(&uevent_sockets,s,link)if(s->bound&&s->port==port)return s;
    return NULL;
}
static uint32_t process_port(void) { lock(&pids_lock);uint32_t port=tgid_view(current->tgid,current->pidns);unlock(&pids_lock);return port; }
static void autobind(struct uevent_socket *s,uint32_t preferred) {
    if(s->bound)return;
    uint32_t candidate=preferred;
    while(!candidate||lookup(candidate))candidate=next_port--;
    s->port=candidate;s->bound=true;
}
static int read_address(addr_t address,unsigned size,struct sockaddr_nl_ *name) {
    if(size<sizeof(*name))return _EINVAL;
    if(user_get(address,*name))return _EFAULT;
    return name->family==AF_NETLINK_?0:_EINVAL;
}
static void marker(struct uevent_socket *s,bool before) {
    bool after=s->head||s->error;
    if(!before&&after){ssize_t ignored=send(s->fd->socket.netlink_peer_fd,"u",1,MSG_DONTWAIT);(void)ignored;}
    if(before&&!after){char token;ssize_t ignored=recv(s->fd->real_fd,&token,1,MSG_DONTWAIT);(void)ignored;}
}
fd_t uevent_create(unsigned type) {
    if((type&15)!=SOCK_RAW_&&(type&15)!=SOCK_DGRAM_)return _ESOCKTNOSUPPORT;
    if(type&~(15|SOCK_NONBLOCK_|SOCK_CLOEXEC_))return _EINVAL;
    int pair[2];if(socketpair(AF_UNIX,SOCK_DGRAM,0,pair))return errno_map();
    // socket_fdops exposes the native descriptor's status flags through
    // F_GETFL. Only the private marker writer is unconditionally nonblocking;
    // the guest endpoint must retain the requested blocking semantics.
    if(fcntl(pair[0],F_SETFL,(type&SOCK_NONBLOCK_)?O_NONBLOCK:0)<0||fcntl(pair[1],F_SETFL,O_NONBLOCK)<0){int error=errno_map();close(pair[0]);close(pair[1]);return error;}
    struct fd *fd=adhoc_fd_create(&socket_fdops);struct uevent_socket *s=calloc(1,sizeof(*s));
    if(!fd||!s){if(fd){fd->real_fd=pair[0];fd->socket.netlink_peer_fd=-1;fd_close(fd);}else close(pair[0]);close(pair[1]);free(s);return _ENOMEM;}
    fd->real_fd=pair[0];fd->type=S_IFSOCK;fd->stat.mode=S_IFSOCK|0666;fd->stat.uid=current->euid;fd->stat.gid=current->egid;fd->socket.domain=AF_NETLINK_;fd->socket.type=type&15;fd->socket.protocol=NETLINK_KOBJECT_UEVENT_;fd->socket.netlink_peer_fd=pair[1];fd->socket.uevent=s;
    s->fd=fd;s->rcvbuf=256*1024;s->sndbuf=128*1024;
    lock(&uevent_lock);s->generation=++next_generation;list_add(&uevent_sockets,&s->link);unlock(&uevent_lock);
    return f_install(fd,type&~15);
}
int uevent_bind(struct fd *fd,addr_t address,unsigned size) {
    struct sockaddr_nl_ name;int error=read_address(address,size,&name);if(error)return error;
    struct uevent_socket *s=fd->socket.uevent;uint32_t preferred=process_port();lock(&uevent_lock);
    if(s->bound&&name.pid!=s->port)error=_EINVAL;
    else if(name.pid&&lookup(name.pid)&&lookup(name.pid)!=s)error=_EADDRINUSE;
    else{if(!s->bound&&name.pid){s->port=name.pid;s->bound=true;}autobind(s,preferred);s->groups=name.groups;}
    unlock(&uevent_lock);return error;
}
int uevent_connect(struct fd *fd,addr_t address,unsigned size) {
    uint16_t family;if(size<sizeof(family))return _EINVAL;if(user_get(address,family))return _EFAULT;
    struct uevent_socket *s=fd->socket.uevent;
    if(!family){lock(&uevent_lock);s->connected=false;s->destination=(struct sockaddr_nl_){0};unlock(&uevent_lock);return 0;}
    struct sockaddr_nl_ name;int error=read_address(address,size,&name);if(error)return error;
    if((name.pid||name.groups)&&(!superuser()||current->userns))return _EPERM;
    uint32_t preferred=process_port();lock(&uevent_lock);autobind(s,preferred);s->connected=true;s->destination=name;unlock(&uevent_lock);return 0;
}
int uevent_name(struct fd *fd,addr_t address,addr_t length,bool peer) {
    uint32_t capacity;if(user_get(length,capacity))return _EFAULT;
    struct uevent_socket *s=fd->socket.uevent;lock(&uevent_lock);
    struct sockaddr_nl_ name=peer?s->destination:(struct sockaddr_nl_){.family=AF_NETLINK_,.pid=s->port,.groups=s->groups};
    bool connected=s->connected;unlock(&uevent_lock);if(peer&&!connected)return _ENOTCONN;
    if(capacity>sizeof(name))capacity=sizeof(name);if(capacity&&user_write(address,&name,capacity))return _EFAULT;
    capacity=sizeof(name);return user_put(length,capacity)?_EFAULT:0;
}
static int64_t monotonic_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int64_t deadline(struct timespec t){int64_t n=monotonic_ns();if(!t.tv_sec&&!t.tv_nsec)return INT64_MAX;if(t.tv_sec>(INT64_MAX-n-t.tv_nsec)/1000000000)return INT64_MAX;return n+(int64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int pause_io(int64_t until) {
    if(current->exiting||current->group->doing_group_exit)return _EINTR;
    int64_t remaining=until-monotonic_ns();if(remaining<=0)return _EAGAIN;
    if(remaining>50000000)remaining=50000000;
    struct timespec delay={.tv_nsec=remaining};int error=wait_for(&uevent_changed,&uevent_lock,&delay);
    return error==_ETIMEDOUT?0:error;
}
static int enqueue(struct uevent_socket *receiver,const void *bytes,size_t size,struct sockaddr_nl_ sender,struct ucred_ credentials,uint32_t group) {
    size_t kept=lr_bpf_run(receiver->filter,receiver->filter_len,bytes,size);if(!kept)return 0;if(kept>size)kept=size;
    size_t charge=sizeof(struct uevent_packet)+kept;
    if(charge>receiver->rcvbuf||receiver->queued>receiver->rcvbuf-charge||charge>UEVENT_ALL_QUEUES-all_queued)return _EAGAIN;
    struct uevent_packet *p=malloc(charge);if(!p)return _ENOMEM;
    *p=(struct uevent_packet){.size=kept,.sender=sender,.credentials=credentials,.group=group};if(kept)memcpy(p->bytes,bytes,kept);
    bool before=receiver->head||receiver->error;
    if(!before&&send(receiver->fd->socket.netlink_peer_fd,"u",1,MSG_DONTWAIT)!=1){int error=errno_map();free(p);return error;}
    if(receiver->tail)receiver->tail->next=p;else receiver->head=p;receiver->tail=p;receiver->queued+=charge;all_queued+=charge;notify(&uevent_changed);return 0;
}
static int transmit(struct fd *fd,const void *bytes,size_t size,int flags,const struct sockaddr_nl_ *destination) {
    if(flags&~(MSG_DONTWAIT_|MSG_NOSIGNAL_))return _EOPNOTSUPP;
    if(!size)return _ENODATA;
    struct uevent_socket *s=fd->socket.uevent;uint32_t preferred=process_port();lock(&uevent_lock);
    if(size>UEVENT_MAX_MESSAGE||size>s->sndbuf-32){unlock(&uevent_lock);return _EMSGSIZE;}
    struct sockaddr_nl_ dst=destination?*destination:s->connected?s->destination:(struct sockaddr_nl_){.family=AF_NETLINK_};
    if(destination&&(dst.pid||dst.groups)&&(!superuser()||current->userns)){unlock(&uevent_lock);return _EPERM;}
    autobind(s,preferred);uint32_t group=dst.groups?__builtin_ctz(dst.groups)+1:0;dst.groups=group?1U<<(group-1):0;
    struct sockaddr_nl_ sender={.family=AF_NETLINK_,.pid=s->port,.groups=dst.groups};
    struct ucred_ credentials={.pid=current->tgid,.uid=current->uid,.gid=current->gid};
    if(group){struct uevent_socket *receiver;list_for_each_entry(&uevent_sockets,receiver,link){
        if(receiver==s||!receiver->bound||receiver->port==dst.pid||!(receiver->groups&dst.groups))continue;
        int error=enqueue(receiver,bytes,size,sender,credentials,group);
        if(error){bool before=receiver->head||receiver->error;if(!receiver->no_enobufs)receiver->error=_ENOBUFS;marker(receiver,before);if(s->broadcast_error){unlock(&uevent_lock);return _ENOBUFS;}}
    }}
    // There is no host-kernel endpoint. Multicast may have delivered before
    // kernel port 0 refuses the unicast leg, as libudev expects and handles.
    if(!dst.pid){unlock(&uevent_lock);return _ECONNREFUSED;}
    int64_t until=deadline(s->sndtimeo);uint64_t generation=0;int error;
    for(;;){
        struct uevent_socket *receiver=lookup(dst.pid);
        if(!receiver||(generation&&generation!=receiver->generation)){error=_ECONNREFUSED;break;}generation=receiver->generation;
        sender.groups=0;error=enqueue(receiver,bytes,size,sender,credentials,0);
        if(error!=_EAGAIN||(flags&MSG_DONTWAIT_)||(fd_getflags(fd)&O_NONBLOCK_))break;
        error=pause_io(until);if(error)break;
    }
    unlock(&uevent_lock);return error?error:(int)size;
}
static int receive(struct fd *fd,void *buffer,size_t capacity,int flags,struct uevent_packet *info) {
    if(flags&~(MSG_DONTWAIT_|MSG_PEEK_|MSG_TRUNC_|0x40000000))return _EOPNOTSUPP;
    struct uevent_socket *s=fd->socket.uevent;lock(&uevent_lock);int64_t until=deadline(s->rcvtimeo);
    for(;;){
        if(s->error){bool before=s->head||s->error;int error=s->error;s->error=0;marker(s,before);unlock(&uevent_lock);return error;}
        struct uevent_packet *p=s->head;
        if(p){*info=*p;size_t copied=p->size<capacity?p->size:capacity;if(copied)memcpy(buffer,p->bytes,copied);
            if(!(flags&MSG_PEEK_)){s->head=p->next;if(!s->head)s->tail=NULL;size_t charge=sizeof(*p)+p->size;s->queued-=charge;all_queued-=charge;free(p);marker(s,true);notify(&uevent_changed);}
            unlock(&uevent_lock);return (flags&MSG_TRUNC_)?info->size:copied;
        }
        if((flags&MSG_DONTWAIT_)||(fd_getflags(fd)&O_NONBLOCK_)){unlock(&uevent_lock);return _EAGAIN;}
        int error=pause_io(until);if(error){unlock(&uevent_lock);return error;}
    }
}
static int scatter_shape(struct uevent_msghdr *m,struct uevent_iovec *iov,size_t *total) {
    if(m->msg_iovlen>1024)return _EMSGSIZE;*total=0;
    for(size_t i=0;i<m->msg_iovlen;i++){if(user_get(m->msg_iov+i*sizeof(*iov),iov[i]))return _EFAULT;if(iov[i].len>SIZE_MAX-*total)return _EMSGSIZE;*total+=iov[i].len;}
    return 0;
}
int uevent_sendto(struct fd *fd,addr_t address,size_t size,int flags,addr_t name,unsigned namelen) {
    if(size>UEVENT_MAX_MESSAGE)return _EMSGSIZE;struct sockaddr_nl_ dst;int error=0;if(name&&(error=read_address(name,namelen,&dst)))return error;
    void *buffer=malloc(size?:1);if(!buffer)return _ENOMEM;if(size&&user_read(address,buffer,size))error=_EFAULT;else error=transmit(fd,buffer,size,flags,name?&dst:NULL);free(buffer);return error;
}
int uevent_sendmsg(struct fd *fd,addr_t address,int flags) {
    struct uevent_msghdr m;if(user_get(address,m))return _EFAULT;
    struct sockaddr_nl_ dst;int error;if(m.msg_name&&(error=read_address(m.msg_name,m.msg_namelen,&dst)))return error;
    // Explicit credentials are accepted only when they identify the caller.
    // No sender can forge kernel port 0 or another task's credentials.
    if(m.msg_controllen){
        struct uevent_cmsghdr c;struct ucred_ credential;
        if(m.msg_controllen<sizeof(c)+sizeof(credential))return _EINVAL;
        if(user_get(m.msg_control,c))return _EFAULT;
        if(c.level!=SOL_SOCKET_||c.type!=SCM_CREDENTIALS_||c.len!=sizeof(c)+sizeof(credential)||m.msg_controllen>((c.len+sizeof(c.len)-1)&~(sizeof(c.len)-1)))return _EOPNOTSUPP;
        if(user_get(m.msg_control+sizeof(c),credential))return _EFAULT;
        lock(&pids_lock);pid_t_ pid=tgid_view(current->tgid,current->pidns);unlock(&pids_lock);
        if(credential.pid!=pid||credential.uid!=userns_view_uid(current->userns,current->uid)||credential.gid!=userns_view_gid(current->userns,current->gid))return _EPERM;
    }
    struct uevent_iovec iov[1024];size_t total;error=scatter_shape(&m,iov,&total);if(error)return error;if(total>UEVENT_MAX_MESSAGE)return _EMSGSIZE;
    uint8_t *buffer=malloc(total?:1);if(!buffer)return _ENOMEM;size_t copied=0;
    for(size_t i=0;i<m.msg_iovlen;i++){struct uevent_iovec v=iov[i];if(v.len&&user_read(v.base,buffer+copied,v.len)){error=_EFAULT;break;}copied+=v.len;}
    if(!error&&copied!=total)error=_EFAULT;
    if(!error)error=transmit(fd,buffer,total,flags,m.msg_name?&dst:NULL);free(buffer);return error;
}
int uevent_recvfrom(struct fd *fd,addr_t address,size_t size,int flags,addr_t name,addr_t length) {
    uint32_t capacity=0;if(name&&(!length||user_get(length,capacity)))return _EFAULT;
    size_t amount=size<UEVENT_MAX_MESSAGE?size:UEVENT_MAX_MESSAGE;uint8_t *buffer=malloc(amount?:1);if(!buffer)return _ENOMEM;
    struct uevent_packet info;int n=receive(fd,buffer,amount,flags,&info);if(n<0){free(buffer);return n;}size_t copied=info.size<amount?info.size:amount;
    int error=copied&&user_write(address,buffer,copied)?_EFAULT:0;free(buffer);
    if(!error&&name){if(capacity>sizeof(info.sender))capacity=sizeof(info.sender);if(capacity&&user_write(name,&info.sender,capacity))error=_EFAULT;capacity=sizeof(info.sender);if(user_put(length,capacity))error=_EFAULT;}
    return error?error:n;
}
static int put_control(struct uevent_msghdr *m,size_t capacity,int level,int type,const void *data,size_t size) {
    size_t full=sizeof(struct uevent_cmsghdr)+size,aligned=(full+sizeof(((struct uevent_cmsghdr *)0)->len)-1)&~(sizeof(((struct uevent_cmsghdr *)0)->len)-1);
    size_t left=capacity-m->msg_controllen;
    if(left<full)m->msg_flags|=MSG_CTRUNC_;
    if(left<sizeof(struct uevent_cmsghdr))return 0;
    size_t length=left<full?left:full;
    struct uevent_cmsghdr c={.len=length,.level=level,.type=type};addr_t base=m->msg_control+m->msg_controllen;
    if(user_put(base,c)||user_write(base+sizeof(c),data,length-sizeof(c)))return _EFAULT;
    m->msg_controllen+=left<aligned?left:aligned;return 0;
}
int uevent_recvmsg(struct fd *fd,addr_t address,int flags) {
    struct uevent_msghdr m;if(user_get(address,m))return _EFAULT;struct uevent_iovec iov[1024];size_t capacity;int error=scatter_shape(&m,iov,&capacity);if(error)return error;
    size_t amount=capacity<UEVENT_MAX_MESSAGE?capacity:UEVENT_MAX_MESSAGE;uint8_t *buffer=malloc(amount?:1);if(!buffer)return _ENOMEM;
    struct uevent_packet info;int n=receive(fd,buffer,amount,flags,&info);if(n<0){free(buffer);return n;}
    size_t copied=0,available=info.size<amount?info.size:amount;
    for(size_t i=0;i<m.msg_iovlen&&copied<available;i++){struct uevent_iovec v=iov[i];size_t len=v.len<available-copied?v.len:available-copied;if(len&&user_write(v.base,buffer+copied,len)){error=_EFAULT;break;}copied+=len;}
    free(buffer);if(error)return error;
    if(m.msg_name){size_t len=m.msg_namelen<sizeof(info.sender)?m.msg_namelen:sizeof(info.sender);if(len&&user_write(m.msg_name,&info.sender,len))return _EFAULT;m.msg_namelen=sizeof(info.sender);}else m.msg_namelen=0;
    size_t control=m.msg_controllen;m.msg_controllen=0;m.msg_flags=capacity<info.size?MSG_TRUNC_:0;
    lock(&uevent_lock);bool credentials=fd->socket.uevent->passcred,packetinfo=fd->socket.uevent->pktinfo;unlock(&uevent_lock);
    if(credentials){struct ucred_ credential=info.credentials;if(current->pidns){lock(&pids_lock);credential.pid=pid_view(credential.pid,current->pidns);unlock(&pids_lock);}credential.uid=userns_view_uid(current->userns,credential.uid);credential.gid=userns_view_gid(current->userns,credential.gid);error=put_control(&m,control,SOL_SOCKET_,SCM_CREDENTIALS_,&credential,sizeof(credential));}
    if(!error&&packetinfo)error=put_control(&m,control,SOL_NETLINK_,3,&info.group,sizeof(info.group));
    if(error)return error;return user_put(address,m)?_EFAULT:n;
}
ssize_t uevent_read(struct fd *fd,void *buffer,size_t size){if(!size)return 0;struct uevent_packet info;return receive(fd,buffer,size,0,&info);}
ssize_t uevent_write(struct fd *fd,const void *buffer,size_t size){return transmit(fd,buffer,size,0,NULL);}
static int install_filter(struct fd *fd,addr_t value,unsigned size) {
    struct uevent_fprog program;if(size!=sizeof(program))return _EINVAL;if(user_get(value,program))return _EFAULT;
    if(!program.count||program.count>4096)return _EINVAL;size_t bytes=program.count*sizeof(struct lr_bpf_insn);
    struct lr_bpf_insn *p=malloc(bytes);if(!p)return _ENOMEM;if(user_read(program.pointer,p,bytes)){free(p);return _EFAULT;}
    int error=lr_bpf_validate(p,program.count);if(error){free(p);return error==2?_EOPNOTSUPP:error==3?_ENOMEM:_EINVAL;}
    lock(&uevent_lock);struct uevent_socket *s=fd->socket.uevent;
    if(s->locked){unlock(&uevent_lock);free(p);return _EPERM;}free(s->filter);s->filter=p;s->filter_len=program.count;unlock(&uevent_lock);return 0;
}
bool uevent_setsockopt(struct fd *fd,unsigned level,unsigned option,addr_t address,unsigned size,int *result) {
    // Native socket timestamps would describe our readiness marker, not the
    // guest event. Do not promise timestamps/low-water marks we do not encode.
    if(level==SOL_SOCKET_&&(option==18||option==19||option==29||option==35||option==37||option==63||option==64||option==65)){*result=_EOPNOTSUPP;return true;}
    if(level!=SOL_NETLINK_&&!(level==SOL_SOCKET_&&(option==SO_PASSCRED_||option==SO_RCVBUF_||option==SO_SNDBUF_||option==32||option==33||option==26||option==27||option==44||option==20||option==21)))return false;
    if(level==SOL_SOCKET_&&option==26){*result=install_filter(fd,address,size);return true;}
    int value=0,error=0;struct timespec timeout={0};
    if(level==SOL_SOCKET_&&(option==20||option==21)){
        struct timeval_ t;if(size<sizeof(t))error=_EINVAL;else if(user_get(address,t))error=_EFAULT;else if(t.sec<0||t.usec<0||t.usec>=1000000)error=_EDOM;else timeout=(struct timespec){.tv_sec=t.sec,.tv_nsec=t.usec*1000};
    }else if(size<sizeof(value))error=_EINVAL;else if(user_get(address,value))error=_EFAULT;
    if(error){*result=error;return true;}
    lock(&uevent_lock);struct uevent_socket *s=fd->socket.uevent;
    if(level==SOL_NETLINK_)switch(option){
        case 1:case 2:if(value<1||value>32)error=_EINVAL;else if(option==1)s->groups|=1U<<(value-1);else s->groups&=~(1U<<(value-1));break;
        case 3:s->pktinfo=!!value;break;case 4:s->broadcast_error=!!value;break;case 5:s->no_enobufs=!!value;break;
        default:error=_ENOPROTOOPT;break;
    }else switch(option){
        case SO_PASSCRED_:s->passcred=!!value;break;
        case 7:case 8:case 32:case 33:
            if((option==32||option==33)&&(!superuser()||current->userns))error=_EPERM;
            else{size_t bytes=value<2304?4608:(size_t)value>UEVENT_MAX_QUEUE/2?UEVENT_MAX_QUEUE:(size_t)value*2;if(option==8||option==33)s->rcvbuf=bytes;else s->sndbuf=bytes;}break;
        case 20:s->rcvtimeo=timeout;break;case 21:s->sndtimeo=timeout;break;
        case 27:if(s->locked)error=_EPERM;else if(!s->filter)error=_ENOENT;else{free(s->filter);s->filter=NULL;s->filter_len=0;}break;
        case 44:if(s->locked&&!value)error=_EPERM;else s->locked=!!value;break;
    }
    unlock(&uevent_lock);*result=error;return true;
}
bool uevent_getsockopt(struct fd *fd,unsigned level,unsigned option,addr_t address,addr_t length,int *result) {
    if(level!=SOL_NETLINK_&&!(level==SOL_SOCKET_&&(option==SO_PASSCRED_||option==SO_RCVBUF_||option==SO_SNDBUF_||option==SO_ERROR_||option==26||option==44||option==20||option==21)))return false;
    uint32_t capacity;if(user_get(length,capacity)){*result=_EFAULT;return true;}
    lock(&uevent_lock);struct uevent_socket *s=fd->socket.uevent;int value=0,error=0;size_t bytes=sizeof(value);const void *data=&value;struct timeval_ timeout={0};
    if(level==SOL_NETLINK_)switch(option){case 3:value=s->pktinfo;break;case 4:value=s->broadcast_error;break;case 5:value=s->no_enobufs;break;case 9:value=s->groups;break;default:error=_ENOPROTOOPT;break;}
    else switch(option){
        case SO_PASSCRED_:value=s->passcred;break;case SO_RCVBUF_:value=s->rcvbuf;break;case SO_SNDBUF_:value=s->sndbuf;break;
        case SO_ERROR_:{bool before=s->head||s->error;value=-s->error;s->error=0;marker(s,before);break;}
        case 44:value=s->locked;break;
        case 20:case 21:{struct timespec t=option==20?s->rcvtimeo:s->sndtimeo;timeout.sec=t.tv_sec;timeout.usec=t.tv_nsec/1000;bytes=sizeof(timeout);data=&timeout;break;}
        case 26: // SO_GET_FILTER length is an instruction count, not bytes.
            if(capacity&&capacity<s->filter_len)error=_EINVAL;
            else if(capacity&&s->filter_len&&user_write(address,s->filter,s->filter_len*sizeof(*s->filter)))error=_EFAULT;
            capacity=s->filter_len;unlock(&uevent_lock);if(!error&&user_put(length,capacity))error=_EFAULT;*result=error;return true;
    }
    if(!error){if(capacity>bytes)capacity=bytes;if(capacity&&user_write(address,data,capacity))error=_EFAULT;}
    unlock(&uevent_lock);if(!error&&user_put(length,capacity))error=_EFAULT;*result=error;return true;
}
int uevent_poll_error(struct fd *fd){lock(&uevent_lock);int result=fd->socket.uevent->error?POLL_ERR:0;unlock(&uevent_lock);return result;}
int uevent_available(struct fd *fd){lock(&uevent_lock);struct uevent_packet *p=fd->socket.uevent->head;int size=p?p->size:0;unlock(&uevent_lock);return size;}
void uevent_close(struct fd *fd){
    struct uevent_socket *s=fd->socket.uevent;if(!s)return;
    lock(&uevent_lock);list_remove(&s->link);while(s->head){struct uevent_packet *p=s->head;s->head=p->next;all_queued-=sizeof(*p)+p->size;free(p);}notify(&uevent_changed);unlock(&uevent_lock);
    free(s->filter);free(s);fd->socket.uevent=NULL;close(fd->socket.netlink_peer_fd);fd->socket.netlink_peer_fd=-1;
}
