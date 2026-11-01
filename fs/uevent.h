#ifndef LR_UEVENT_H
#define LR_UEVENT_H
#include "fs/fd.h"
#include "fs/netlink.h"
#define NETLINK_KOBJECT_UEVENT_ 15
static inline bool uevent_socket(const struct fd *fd) {
    return fd->socket.domain==AF_NETLINK_ && fd->socket.protocol==NETLINK_KOBJECT_UEVENT_;
}
fd_t uevent_create(unsigned type);
int uevent_bind(struct fd *,addr_t,unsigned);
int uevent_connect(struct fd *,addr_t,unsigned);
int uevent_name(struct fd *,addr_t,addr_t,bool);
int uevent_sendto(struct fd *,addr_t,size_t,int,addr_t,unsigned);
int uevent_recvfrom(struct fd *,addr_t,size_t,int,addr_t,addr_t);
int uevent_sendmsg(struct fd *,addr_t,int);
int uevent_recvmsg(struct fd *,addr_t,int);
ssize_t uevent_read(struct fd *,void *,size_t);
ssize_t uevent_write(struct fd *,const void *,size_t);
// Return 1 if handled and set result; otherwise the existing option adapter runs.
bool uevent_setsockopt(struct fd *,unsigned,unsigned,addr_t,unsigned,int *);
bool uevent_getsockopt(struct fd *,unsigned,unsigned,addr_t,addr_t,int *);
int uevent_poll_error(struct fd *);
int uevent_available(struct fd *);
void uevent_close(struct fd *);
#endif
