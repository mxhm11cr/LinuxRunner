// CPU-only Linux udmabuf subset. The exported buffer aliases sealed memfd
// backing; no packet, physical DMA, device fence or GPU capability is invented.
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "fs/dmabuf.h"
#include "fs/real.h"
#include "fs/poll.h"

#define UDMABUF_CREATE_ 0x40187542U
#define UDMABUF_CREATE_LIST_ 0x40087543U
#define DMA_BUF_IOCTL_SYNC_ 0x40086200U
#define BUFFER_LIMIT (128ULL*1024*1024)
#define EXPORT_BUDGET (256ULL*1024*1024)
struct create_request { uint32_t memfd,flags; uint64_t offset,size; };
struct software_export { struct fd *source; uint64_t offset,size; };
static atomic_ullong exported_bytes;
static atomic_ullong next_inode=1;
static const struct fd_ops software_dmabuf_ops;
bool fd_is_software_dmabuf(const struct fd *fd) { return fd && fd->ops==&software_dmabuf_ops; }
static bool reserve_export(uint64_t bytes) {
    unsigned long long old=atomic_load(&exported_bytes);
    do { if(bytes>EXPORT_BUDGET-old)return false; }
    while(!atomic_compare_exchange_weak(&exported_bytes,&old,old+bytes));
    return true;
}
static ssize_t buffer_read(struct fd *UNUSED(fd),void *UNUSED(buf),size_t UNUSED(size)) { return _EINVAL; }
static ssize_t buffer_write(struct fd *UNUSED(fd),const void *UNUSED(buf),size_t UNUSED(size)) { return _EINVAL; }
static ssize_t buffer_pread(struct fd *UNUSED(fd),void *UNUSED(buf),size_t UNUSED(size),off_t UNUSED(offset)) { return _EINVAL; }
static ssize_t buffer_pwrite(struct fd *UNUSED(fd),const void *UNUSED(buf),size_t UNUSED(size),off_t UNUSED(offset)) { return _EINVAL; }
static off_t_ buffer_seek(struct fd *fd,off_t_ offset,int whence) {
    struct software_export *b=fd->data;
    if(offset || (whence!=LSEEK_SET && whence!=LSEEK_END))return _EINVAL;
    return fd->offset=whence==LSEEK_END?b->size:0;
}
static int buffer_map(struct fd *fd,struct mem *mem,page_t start,pages_t pages,off_t offset,int prot,int flags) {
    struct software_export *b=fd->data;
    uint64_t bytes=(uint64_t)pages*PAGE_SIZE;
    if(!(flags&MMAP_SHARED) || offset<0 || (uint64_t)offset>b->size || bytes>b->size-(uint64_t)offset)return _EINVAL;
    // realfs_mmap already preserves 4K guest offsets on 16K host pages.
    return realfs_mmap(b->source,mem,start,pages,b->offset+(uint64_t)offset,prot,flags);
}
static ssize_t buffer_ioctl_size(int command) { return (uint32_t)command==DMA_BUF_IOCTL_SYNC_?0:-1; }
static int buffer_ioctl(struct fd *UNUSED(fd),int command,void *address) {
    if((uint32_t)command!=DMA_BUF_IOCTL_SYNC_)return _ENOTTY;
    uint64_t flags;
    if(user_get((addr_t)(uintptr_t)address,flags))return _EFAULT;
    if((flags&~7ULL) || !(flags&3))return _EINVAL;
    // Every mapping is backed by the same coherent host pages. This is a real
    // CPU memory barrier; there is no asynchronous device engine to wait on.
    atomic_thread_fence(memory_order_seq_cst);
    return 0;
}
static int buffer_poll(struct fd *UNUSED(fd)) { return POLL_READ|POLL_WRITE; }
static int buffer_close(struct fd *fd) {
    struct software_export *b=fd->data;
    if(b){fd_close(b->source);atomic_fetch_sub(&exported_bytes,b->size);free(b);}
    return 0;
}
static const struct fd_ops software_dmabuf_ops={
    .read=buffer_read,.write=buffer_write,.pread=buffer_pread,.pwrite=buffer_pwrite,
    .lseek=buffer_seek,.mmap=buffer_map,.poll=buffer_poll,
    .ioctl_size=buffer_ioctl_size,.ioctl=buffer_ioctl,.close=buffer_close,
};
// ISH_DMABUF_TRACE=1: report why a request is refused (development only).
static void trace_refusal(const struct create_request *r,const char *why,long long source_size,unsigned seals) {
    static int on=-1;
    if(on<0){const char *v=getenv("ISH_DMABUF_TRACE");on=v&&v[0]=='1';}
    if(on)printk("DMABUF_REFUSED why=%s flags=%#x offset=%llu size=%llu source_size=%lld seals=%#x pid=%d\n",why,r->flags,
        (unsigned long long)r->offset,(unsigned long long)r->size,source_size,seals,current->pid);
}
bool software_dmabuf_source(const struct fd *fd,struct fd **source,uint64_t *offset,uint64_t *size) {
    if(!fd_is_software_dmabuf(fd))return false;
    const struct software_export *b=fd->data;
    *source=b->source;*offset=b->offset;*size=b->size;
    return true;
}
static int export_buffer(struct create_request r) {
    if(r.flags&~1U || !r.size || r.offset%PAGE_SIZE || r.size%PAGE_SIZE ||
            r.size>BUFFER_LIMIT || r.offset>INT64_MAX-r.size){trace_refusal(&r,"request",-1,0);return _EINVAL;}
    struct fd *source=f_get_retain(r.memfd);
    if(!source)return _EBADFD;
    return software_dmabuf_export(source,r.offset,r.size,r.flags);
}
// Takes over the caller's reference to source (released on every failure).
int software_dmabuf_export(struct fd *source,uint64_t offset,uint64_t size,unsigned flags) {
    struct create_request r={.flags=flags,.offset=offset,.size=size};
    if(!source->memfd){fd_close(source);return _EBADFD;}
    lock(&source->memfd->lock);
    int error=0;struct stat st;
    // Linux udmabuf refuses write-sealed memfds (pages must stay writable).
    if(!(source->memfd->seals&F_SEAL_SHRINK_) || (source->memfd->seals&F_SEAL_ANY_WRITE_))error=_EINVAL;
    // Restrict the software subset to writable descriptions: opening the
    // control node must never turn someone else's read-only file into a writer.
    else if((fd_getflags(source)&O_ACCMODE_)!=O_RDWR_)error=_EACCES;
    else if(fstat(source->real_fd,&st))error=errno_map();
    // Linux pins page n while n*PAGE_SIZE < i_size (shmem_get_folio), so the
    // final partial page of the memfd is exportable.
    else if(r.offset+r.size>(((uint64_t)st.st_size+PAGE_SIZE-1)/PAGE_SIZE)*PAGE_SIZE)error=_EINVAL;
    unlock(&source->memfd->lock);
    if(error){trace_refusal(&r,error==_EACCES?"access":!(source->memfd->seals&F_SEAL_SHRINK_)?"not-shrink-sealed":"range",
        error==_EACCES||!(source->memfd->seals&F_SEAL_SHRINK_)?-1:(long long)st.st_size,source->memfd->seals);fd_close(source);return error;}
    if(!reserve_export(r.size)){fd_close(source);return _ENOMEM;}
    struct software_export *b=malloc(sizeof(*b));
    if(!b){atomic_fetch_sub(&exported_bytes,r.size);fd_close(source);return _ENOMEM;}
    *b=(struct software_export){source,r.offset,r.size};
    struct fd *out=adhoc_fd_create(&software_dmabuf_ops);
    if(!out){free(b);atomic_fetch_sub(&exported_bytes,r.size);fd_close(source);return _ENOMEM;}
    // An anonymous descriptor has no native fd of its own. Retaining it in a
    // VMA and SCM_RIGHTS keeps the source alive, including after all closes.
    out->data=b;out->real_fd=-1;out->flags=O_RDWR_;out->type=S_IFREG;
    out->stat=(struct statbuf){.mode=S_IFREG|0600,.nlink=1,.uid=current->euid,.gid=current->egid,
        .inode=atomic_fetch_add(&next_inode,1),.size=r.size,.blksize=PAGE_SIZE,.blocks=r.size/512};
    return f_install(out,r.flags&1?O_CLOEXEC_:0);
}
static int control_open(int UNUSED(major),int minor,struct fd *UNUSED(fd)) {
    const char *enabled=getenv("ISH_SOFTWARE_DMABUF");
    if(minor!=LR_UDMABUF_MINOR || !enabled || enabled[0]!='1' || enabled[1])return _ENXIO;
    return 0;
}
static ssize_t control_ioctl_size(int command) {
    return (uint32_t)command==UDMABUF_CREATE_ || (uint32_t)command==UDMABUF_CREATE_LIST_?0:-1;
}
static int control_ioctl(struct fd *UNUSED(fd),int command,void *address) {
    if((uint32_t)command==UDMABUF_CREATE_LIST_)return _EOPNOTSUPP;
    if((uint32_t)command!=UDMABUF_CREATE_)return _ENOTTY;
    struct create_request r;
    if(user_get((addr_t)(uintptr_t)address,r))return _EFAULT;
    return export_buffer(r);
}
struct dev_ops software_udmabuf_dev={.open=control_open,.fd={.ioctl_size=control_ioctl_size,.ioctl=control_ioctl}};
