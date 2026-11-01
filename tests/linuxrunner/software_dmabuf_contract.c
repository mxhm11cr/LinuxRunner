#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/futex.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
static int export(int control,int fd,uint64_t offset,uint64_t size,unsigned flags){struct udmabuf_create c={fd,flags,offset,size};return ioctl(control,UDMABUF_CREATE,&c);}
int main(int argc,char **argv){
 assert(!getuid());alarm(15);char path[96];snprintf(path,sizeof path,"/tmp/linuxrunner-udmabuf-%ld",(long)getpid());
 assert(!mknod(path,S_IFCHR|0600,makedev(10,254)));int control=open(path,O_RDWR|O_CLOEXEC);
 if(argc==2&&!strcmp(argv[1],"--disabled")){assert(control<0&&errno==ENXIO);assert(!unlink(path));puts("SOFTWARE_DMABUF_DISABLED_PASS no opt-in, no control access");return 0;}
 assert(control>=0);assert(!unlink(path));
 int fd=memfd_create("cpu-export-contract",MFD_ALLOW_SEALING|MFD_CLOEXEC);assert(fd>=0);const size_t p=4096;assert(!ftruncate(fd,6*p));
 assert(export(control,fd,p,2*p,1)<0&&errno==EINVAL);assert(!fcntl(fd,F_ADD_SEALS,F_SEAL_SHRINK));
 char procpath[96];snprintf(procpath,sizeof procpath,"/proc/self/fd/%d",fd);int ro=open(procpath,O_RDONLY);assert(ro>=0);assert(export(control,ro,0,p,0)<0&&errno==EACCES);close(ro);
 assert(export(control,-1,0,p,0)<0&&errno==EBADFD);assert(export(control,control,0,p,0)<0&&errno==EBADFD);
 assert(export(control,fd,1,p,0)<0&&errno==EINVAL);assert(export(control,fd,0,0,0)<0&&errno==EINVAL);
 assert(export(control,fd,0,p,2)<0&&errno==EINVAL);assert(export(control,fd,6*p,p,0)<0&&errno==EINVAL);
 assert(export(control,fd,UINT64_MAX-4095,p,0)<0&&errno==EINVAL);
 // Linux exports the final partial page of a memfd (page n is valid while
 // n*PAGE_SIZE < i_size): a 1 KiB memfd yields one page that aliases it.
 {int small=memfd_create("partial-page",MFD_ALLOW_SEALING|MFD_CLOEXEC);assert(small>=0&&!ftruncate(small,1024));
  assert(pwrite(small,"partial",7,1000)==7);assert(!fcntl(small,F_ADD_SEALS,F_SEAL_SHRINK));
  assert(export(control,small,0,2*p,0)<0&&errno==EINVAL);int one=export(control,small,0,p,1);assert(one>=0);
  unsigned char *view=mmap(NULL,p,PROT_READ,MAP_SHARED,one,0);assert(view!=MAP_FAILED&&!memcmp(view+1000,"partial",7));
  assert(!munmap(view,p));close(one);close(small);}
 unsigned char *original=mmap(NULL,6*p,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(original!=MAP_FAILED);original[p]=0x51;
 // _IOW must work with a genuinely read-only request page, without writeback.
 struct udmabuf_create *request=mmap(NULL,p,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(request!=MAP_FAILED);
 *request=(struct udmabuf_create){fd,UDMABUF_FLAGS_CLOEXEC,p,2*p};assert(!mprotect(request,p,PROT_READ));int buffer=ioctl(control,UDMABUF_CREATE,request);assert(buffer>=0);assert(!munmap(request,p));
 assert(fcntl(buffer,F_GETFD)&FD_CLOEXEC);struct stat st;assert(!fstat(buffer,&st)&&S_ISREG(st.st_mode)&&st.st_size==2*p);
 assert(lseek(buffer,0,SEEK_END)==2*p&&lseek(buffer,0,SEEK_SET)==0);assert(lseek(buffer,1,SEEK_SET)<0&&errno==EINVAL);
 char data;assert(read(buffer,&data,1)<0&&errno==EINVAL);assert(write(buffer,"x",1)<0&&errno==EINVAL);assert(ftruncate(buffer,p)<0&&errno==EINVAL);
 assert(fcntl(buffer,F_GET_SEALS)<0&&errno==EINVAL);
 assert(mmap(NULL,p,PROT_READ,MAP_PRIVATE,buffer,0)==MAP_FAILED&&errno==EINVAL);
 assert(mmap(NULL,p,PROT_READ,MAP_SHARED,buffer,2*p)==MAP_FAILED&&errno==EINVAL);
 unsigned char *mapped=mmap(NULL,2*p,PROT_READ|PROT_WRITE,MAP_SHARED,buffer,0);assert(mapped!=MAP_FAILED&&mapped[0]==0x51);
 uint64_t sync=DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW;assert(!ioctl(buffer,DMA_BUF_IOCTL_SYNC,&sync));mapped[p+17]=0xa4;assert(original[2*p+17]==0xa4);
 sync=DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW;assert(!ioctl(buffer,DMA_BUF_IOCTL_SYNC,&sync));sync=0;assert(ioctl(buffer,DMA_BUF_IOCTL_SYNC,&sync)<0&&errno==EINVAL);sync=8|1;assert(ioctl(buffer,DMA_BUF_IOCTL_SYNC,&sync)<0&&errno==EINVAL);
 assert(mremap(mapped,2*p,3*p,MREMAP_MAYMOVE)==MAP_FAILED&&errno==EFAULT);
 assert(syscall(SYS_futex,mapped,FUTEX_WAKE,1,NULL,NULL,0)<0&&errno==EFAULT);
 assert(syscall(SYS_futex,mapped,FUTEX_WAKE_PRIVATE,1,NULL,NULL,0)==0);
 assert(!ftruncate(fd,128*1024*1024));int large=export(control,fd,0,128*1024*1024,0);assert(large>=0);
 assert(export(control,fd,0,128*1024*1024,0)<0&&errno==ENOMEM);close(large);
 large=export(control,fd,0,128*1024*1024,0);assert(large>=0);close(large);
 int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));pid_t pid=fork();assert(pid>=0);
 if(!pid){close(sockets[0]);close(buffer);close(fd);close(control);assert(!munmap(original,6*p)&&!munmap(mapped,2*p));char c;struct iovec iov={&c,1};char ancillary[CMSG_SPACE(sizeof(int))];struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ancillary,.msg_controllen=sizeof ancillary};assert(recvmsg(sockets[1],&msg,MSG_CMSG_CLOEXEC)==1);struct cmsghdr *cm=CMSG_FIRSTHDR(&msg);assert(cm&&cm->cmsg_level==SOL_SOCKET&&cm->cmsg_type==SCM_RIGHTS);int passed;memcpy(&passed,CMSG_DATA(cm),sizeof passed);assert(passed>=0&&(fcntl(passed,F_GETFD)&FD_CLOEXEC));unsigned char *alias=mmap(NULL,2*p,PROT_READ|PROT_WRITE,MAP_SHARED,passed,0);assert(alias!=MAP_FAILED&&alias[p+17]==0xa4);alias[31]=0x72;close(passed);assert(alias[31]==0x72);assert(!munmap(alias,2*p));_exit(0);}
 close(sockets[1]);char c='b';struct iovec iov={&c,1};char ancillary[CMSG_SPACE(sizeof(int))]={0};struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=ancillary,.msg_controllen=sizeof ancillary};struct cmsghdr *cm=CMSG_FIRSTHDR(&msg);cm->cmsg_level=SOL_SOCKET;cm->cmsg_type=SCM_RIGHTS;cm->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(cm),&buffer,sizeof buffer);assert(sendmsg(sockets[0],&msg,0)==1);int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));assert(mapped[31]==0x72&&original[p+31]==0x72);close(sockets[0]);
 close(fd);close(buffer);close(control);mapped[0]=0x29;assert(original[p]==0x29);assert(!munmap(mapped,2*p)&&!munmap(original,6*p));alarm(0);
 puts("SOFTWARE_DMABUF_PASS sealed memfd alias, offsets, FD lifetime/SCM, CPU sync, no private maps/growth/shared futex; no physical DMA");return 0;
}
