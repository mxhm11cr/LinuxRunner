/* Freestanding ARM64 Linux contract; same license as LinuxRunner. */
#include "kernel/capture_shm.h"
typedef unsigned long size_t;
typedef unsigned long long u64;
static char **environment;
static long call(long nr, long a, long b, long c, long d, long e) {
    register long x0 __asm__("x0")=a,x1 __asm__("x1")=b,x2 __asm__("x2")=c;
    register long x3 __asm__("x3")=d,x4 __asm__("x4")=e,x8 __asm__("x8")=nr;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x8):"memory","cc");
    return x0;
}
static size_t length(const char *s){size_t n=0;while(s[n])n++;return n;}
static void text(const char *s){call(64,1,(long)s,length(s),0,0);}
static void number(long n){char b[32];unsigned i=0;unsigned long v=n<0?-n:n;if(n<0)text("-");do{b[i++]='0'+v%10;v/=10;}while(v);while(i)call(64,1,(long)&b[--i],1,0,0);}
static void stop(int code){call(93,code,0,0,0,0);for(;;){}}
static void fail(unsigned line,long result){text("CAPTURE_SHM_FAIL line=");number(line);text(" result=");number(result);text("\n");stop(1);}
#define CHECK(x) do{long r=(x);if(!r)fail(__LINE__,r);}while(0)
#define OK(x) do{long r=(x);if(r<0)fail(__LINE__,r);}while(0)
static long allocate(void){return call(194,0,4096,0600,0,0);}
static void enable(void){OK(call(167,LR_PR_CAPTURE_SHM,LR_CAPTURE_SHM_VERSION,0,0,0));}
static void nap(void){long t[2]={0,100000000};OK(call(101,(long)t,0,0,0,0));}
static void pipe_create(int p[2]){OK(call(59,(long)p,0,0,0,0));}
static void close_fd(int fd){OK(call(57,fd,0,0,0,0));}
static void send_id(int fd,int id){CHECK(call(64,fd,(long)&id,4,0,0)==4);}
static int receive_id(int fd){int id=-1;CHECK(call(63,fd,(long)&id,4,0,0)==4);return id;}
static void reap_status(long pid,int expected){int status=-1;CHECK(call(260,pid,(long)&status,0,0,0)==pid);CHECK(status==expected);}
static void reap(long pid){reap_status(pid,0);}
static long spawn(void){return call(220,17,0,0,0,0);}
static void kill_child(long pid){OK(call(129,pid,9,0,0,0));reap_status(pid,9);}
static int valid(int id){unsigned char stat[128];return call(195,id,2,(long)stat,0,0)==0;}
static void remove_id(int id){OK(call(195,id,0,0,0,0));}
static void hold(void){char b;for(;;)call(63,0,(long)&b,1,0,0);}
static int same(const char *a,const char *b){while(*a&&*a==*b){a++;b++;}return *a==*b;}
static void exec(char **argv){call(221,(long)argv[0],(long)argv,(long)environment,0,0);fail(__LINE__,-1);}
static u64 value(const char *data,const char *key){
    for(const char *p=data;*p;p++){
        const char *a=p,*b=key;while(*b&&*a==*b){a++;b++;}
        if(!*b&&*a=='='){u64 n=0;for(a++;*a>='0'&&*a<='9';a++)n=n*10+*a-'0';return n;}
    }
    fail(__LINE__,-1);return 0;
}
struct census{u64 segments,fds,scopes,bytes,attachments,close_failures;};
static struct census census(void){
    char buf[512];long fd=call(56,-100,(long)"/proc/ish/shm",0,0,0);OK(fd);
    long n=call(63,fd,(long)buf,sizeof(buf)-1,0,0);OK(n);buf[n]=0;close_fd(fd);
    return(struct census){value(buf,"segments"),value(buf,"backing_fds"),value(buf,"scopes"),value(buf,"bytes"),value(buf,"attachments"),value(buf,"close_failures")};
}
static void equal_census(struct census a,struct census b){CHECK(a.segments==b.segments&&a.fds==b.fds&&a.scopes==b.scopes&&a.bytes==b.bytes&&a.attachments==b.attachments&&a.close_failures==b.close_failures);}

static unsigned char thread_stack[65536] __attribute__((aligned(16)));
static volatile unsigned thread_ready;
static int thread_control;
static int thread_allocates;
static void worker(void){if(thread_allocates){int id=allocate();OK(id);send_id(thread_control,id);hold();}
__atomic_store_n(&thread_ready,1,__ATOMIC_RELEASE);(void)receive_id(thread_control);}
extern long start_thread(void *stack,void (*fn)(void));
__asm__(".text\n.align 2\n.global start_thread\n.type start_thread,%function\nstart_thread:\n"
        "sub x0,x0,#16\nstr x1,[x0]\nmov x1,x0\nmov x0,#0xf00\nmovk x0,#1,lsl #16\n"
        "mov x2,#0\nmov x3,#0\nmov x4,#0\nmov x8,#220\nsvc #0\ncbz x0,1f\nret\n"
        "1: ldr x2,[sp]\nblr x2\nmov x0,#0\nmov x8,#93\nsvc #0\nb .\n");

__attribute__((used)) long contract_start(unsigned long *stack){
    long argc=stack[0];char **argv=(char **)(stack+1);environment=argv+argc+1;
    if(argc==2&&same(argv[1],"--allocate")){int id=allocate();OK(id);send_id(1,id);hold();}
    if(argc==3&&same(argv[1],"-noshm")&&same(argv[2],"--fallback"))return 0;
    if(argc!=1)fail(__LINE__,-64);
    struct census baseline=census();
    CHECK(call(167,LR_PR_CAPTURE_SHM,0,0,0,0)==-22);
    CHECK(call(167,LR_PR_CAPTURE_SHM,LR_CAPTURE_SHM_VERSION,1,0,0)==-22);
    equal_census(baseline,census());
    // Ordinary unmarked IPC_PRIVATE objects outlive their creator.
    int p[2];pipe_create(p);long child=spawn();OK(child);
    if(!child){int id=allocate();OK(id);send_id(p[1],id);stop(0);}
    int ordinary=receive_id(p[0]);reap(child);CHECK(valid(ordinary));close_fd(p[0]);close_fd(p[1]);
    struct census with_sentinel=census();
    // No attachment, attached, exec, and SIGKILL all reclaim within this app.
    for(unsigned i=0;i<64;i++){
        pipe_create(p);int control[2];pipe_create(control);child=spawn();OK(child);
        if(!child){
            OK(call(24,p[1],1,0,0,0));OK(call(24,control[0],0,0,0,0));
            if(i%3==2){char *a[]={"/bin/capture-shm-guard","/bin/capture-contract","--allocate",0};exec(a);}
            enable();enable();int id=allocate();OK(id);
            if(i%3==1){long memory=call(196,id,0,0,0,0);OK(memory);*(volatile unsigned *)memory=0x12345678;}
            send_id(1,id);hold();
        }
        int id=receive_id(p[0]);CHECK(valid(id));kill_child(child);CHECK(!valid(id));
        close_fd(p[0]);close_fd(p[1]);close_fd(control[0]);close_fd(control[1]);equal_census(with_sentinel,census());CHECK(valid(ordinary));
    }
    // Only future private objects are owned. Keyed allocations and a forked
    // process's new private object retain ordinary Linux persistence.
    pipe_create(p);child=spawn();OK(child);
    if(!child){
        int before=allocate();OK(before);send_id(p[1],before);enable();
        int key=0x4c520000;long keyed;do{keyed=call(194,key++,4096,01000|02000|0600,0,0);}while(keyed==-17);OK(keyed);send_id(p[1],keyed);
        long grandchild=spawn();OK(grandchild);
        if(!grandchild){int id=allocate();OK(id);send_id(p[1],id);stop(0);}
        reap(grandchild);stop(0);
    }
    int before=receive_id(p[0]),keyed=receive_id(p[0]),forked=receive_id(p[0]);reap(child);
    CHECK(valid(before)&&valid(keyed)&&valid(forked));remove_id(before);remove_id(keyed);remove_id(forked);close_fd(p[0]);close_fd(p[1]);equal_census(with_sentinel,census());
    // Another process's attachment survives owner death, then final detach
    // reclaims the marked object and its backing descriptor.
    pipe_create(p);int control[2];pipe_create(control);child=spawn();OK(child);
    if(!child){enable();int id=allocate();OK(id);send_id(p[1],id);(void)receive_id(control[0]);stop(0);}
    int attached=receive_id(p[0]);long memory=call(196,attached,0,0,0,0);OK(memory);*(volatile unsigned *)memory=73;
    kill_child(child);CHECK(valid(attached)&&*(volatile unsigned *)memory==73);OK(call(197,memory,0,0,0,0));CHECK(!valid(attached));
    close_fd(p[0]);close_fd(p[1]);close_fd(control[0]);close_fd(control[1]);equal_census(with_sentinel,census());
    // Leader exit must not reclaim while its remaining thread is alive.
    pipe_create(p);pipe_create(control);child=spawn();OK(child);
    if(!child){enable();int id=allocate();OK(id);thread_control=control[0];OK(start_thread(thread_stack+sizeof(thread_stack),worker));while(!__atomic_load_n(&thread_ready,__ATOMIC_ACQUIRE))nap();send_id(p[1],id);stop(0);}
    int threaded=receive_id(p[0]);nap();CHECK(valid(threaded));send_id(control[1],1);reap(child);CHECK(!valid(threaded));
    close_fd(p[0]);close_fd(p[1]);close_fd(control[0]);close_fd(control[1]);equal_census(with_sentinel,census());
    // An already-multithreaded process cannot adopt a partially owned scope.
    pipe_create(control);child=spawn();OK(child);
    if(!child){thread_control=control[0];OK(start_thread(thread_stack+sizeof(thread_stack),worker));
        while(!__atomic_load_n(&thread_ready,__ATOMIC_ACQUIRE))nap();
        CHECK(call(167,LR_PR_CAPTURE_SHM,LR_CAPTURE_SHM_VERSION,0,0,0)==-16);
        send_id(control[1],1);stop(0);}
    reap(child);close_fd(control[0]);close_fd(control[1]);equal_census(with_sentinel,census());
    // Kill a whole multithreaded scoped process, including objects allocated
    // by the child thread. The native-thread TLS fallback must also be safe.
    for(unsigned i=0;i<16;i++){
        pipe_create(p);pipe_create(control);child=spawn();OK(child);
        if(!child){enable();thread_control=p[1];thread_allocates=1;
            OK(call(24,control[0],0,0,0,0));
            OK(start_thread(thread_stack+sizeof(thread_stack),worker));hold();}
        int id=receive_id(p[0]);CHECK(valid(id));kill_child(child);CHECK(!valid(id));
        close_fd(p[0]);close_fd(p[1]);close_fd(control[0]);close_fd(control[1]);equal_census(with_sentinel,census());
    }
    // Failed exec retains the scope and is reclaimed on normal exit.
    child=spawn();OK(child);if(!child){enable();OK(allocate());char *a[]={"/missing-capture-target",0};
        CHECK(call(221,(long)a[0],(long)a,(long)environment,0,0)==-2);stop(0);}reap(child);equal_census(with_sentinel,census());
    // Ordinary marked-delete behavior: a late read-only attach and a fork's
    // inherited mapping retain the backing until their final detach.
    int marked=allocate();OK(marked);long first=call(196,marked,0,0,0,0);OK(first);
    *(volatile unsigned *)first=91;remove_id(marked);
    long second=call(196,marked,0,010000,0,0);OK(second);CHECK(*(volatile unsigned *)second==91);
    child=spawn();OK(child);if(!child){CHECK(*(volatile unsigned *)first==91);stop(0);}reap(child);
    OK(call(197,first,0,0,0,0));CHECK(valid(marked));
    OK(call(197,second,0,0,0,0));CHECK(!valid(marked));equal_census(with_sentinel,census());
    child=spawn();OK(child);if(!child){enable();CHECK(call(194,0,0,0600,0,0)==-22);stop(0);}reap(child);equal_census(with_sentinel,census());
    // Probe exit statuses must be distinguishable from execution success.
    child=spawn();OK(child);if(!child){char *a[]={"/bin/capture-shm-guard","--probe",0};exec(a);}reap(child);equal_census(with_sentinel,census());
    child=spawn();OK(child);if(!child){char *a[]={"/bin/guard-unsupported","--probe",0};exec(a);}reap_status(child,77<<8);equal_census(with_sentinel,census());
    // Capability rejection forces the launcher to prepend -noshm and retain
    // command arguments, instead of silently launching unsafe shared capture.
    child=spawn();OK(child);if(!child){char *a[]={"/bin/guard-unsupported","/bin/capture-contract","--fallback",0};exec(a);}reap(child);
    remove_id(ordinary);equal_census(baseline,census());
    text("CAPTURE_SHM_CONTRACT_PASS cycles=64 thread_kills=16 ordinary=preserved fork=isolated exec=retained threads=last-owner attachment=retained fallback=socket resources=baseline\n");
    return 0;
}
__asm__(".text\n.align 2\n.global _start\n.type _start,%function\n_start:\nmov x0,sp\nbl contract_start\nmov x8,#93\nsvc #0\nb .\n");
