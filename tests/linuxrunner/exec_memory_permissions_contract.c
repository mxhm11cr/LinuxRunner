#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
static sigjmp_buf jump;
static volatile sig_atomic_t expected,caught;
// musl rounds mprotect addresses; exercise the actual Linux syscall ABI for
// alignment/length errors instead of confusing libc policy with kernel policy.
static int protect(void *p,size_t size,int prot){return syscall(SYS_mprotect,p,size,prot);}
static void fault(int signal,siginfo_t *info,void *context){(void)context;if(!expected||signal!=SIGSEGV||info->si_code!=SEGV_ACCERR)_exit(90);caught++;expected=0;siglongjmp(jump,1);}
static int execute(void *p){return ((int (*)(void))(uintptr_t)p)();}
static void nx(void *p){int before=caught;if(!sigsetjmp(jump,1)){expected=1;execute(p);assert(!"NX instruction fetch unexpectedly succeeded");}assert(caught==before+1);}
int main(void){alarm(10);struct sigaction sa={.sa_sigaction=fault,.sa_flags=SA_SIGINFO};assert(!sigaction(SIGSEGV,&sa,NULL));const size_t page=4096;uint32_t *p=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(p!=MAP_FAILED);p[0]=0x52800540;p[1]=0xd65f03c0;__builtin___clear_cache((char *)p,(char *)(p+2));nx(p);
 assert(!protect(p,page,PROT_READ|PROT_EXEC));assert(execute(p)==42);assert(!protect(p,page,PROT_READ));nx(p);assert(!protect(p,page,PROT_EXEC));assert(execute(p)==42);
 assert(!protect(p,page,PROT_READ));if(!sigsetjmp(jump,1)){expected=1;*(volatile uint32_t *)p=0;assert(!"write to read-only page succeeded");}
 assert(!protect(p,page,PROT_NONE));if(!sigsetjmp(jump,1)){expected=1;(void)*(volatile uint32_t *)p;assert(!"read from PROT_NONE succeeded");}
 assert(protect((char *)p+1,page,PROT_READ)<0&&errno==EINVAL);assert(protect(p,page,PROT_READ|0x40000000)<0&&errno==EINVAL);assert(protect(p,page,PROT_READ|0x03000000)<0&&errno==EINVAL);assert(protect(p,page,PROT_READ|0x01000000)<0&&errno==EINVAL);assert(protect(p,page,PROT_READ|0x02000000)<0&&errno==EINVAL);
 assert(protect((void *)0xfffffffffffff000ULL,page,PROT_READ)<0&&errno==ENOMEM);assert(protect(p,(size_t)-4096,PROT_READ)<0&&errno==ENOMEM);assert(!protect(p,0,PROT_READ));assert(!munmap((char *)p+page,page));assert(protect(p,2*page,PROT_READ)<0&&errno==ENOMEM);assert(!munmap(p,page));alarm(0);puts("EXEC_MEMORY_PERMISSIONS_PASS NX/RX/execute-only/cached-code revocation/read-only/PROT_NONE/invalid flags/unmapped/overflow");return 0;}
