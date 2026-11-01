#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/ptrace.h"
#include "emu/interrupt.h"
#include "kernel/memory.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/mm.h"
#include "kernel/elf.h"
#include "kernel/fs.h"
#include "fs/stat.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/real.h"
#include "platform/platform.h"

// Optional startup-only observer. It reports numeric fault metadata and does
// not change Linux signal delivery or attempt to recover host corruption.
void (*runtime_instruction_failure)(int pid, uint64_t pc, uint32_t instruction);
void (*runtime_syscall_observer)(int pid, uint64_t pc, uint64_t sp, unsigned syscall);
void (*runtime_guest_fault)(int pid, int ppid, const char *comm, uint64_t pc, uint64_t sp, int signal, uint64_t address, uint32_t instruction);
// Numeric-only optional completion diagnostics. No guest buffers are read.
atomic_bool runtime_syscall_timing_enabled;
void (*runtime_syscall_result_observer)(int, int, unsigned, const uint64_t *, int64_t, uint64_t);
extern volatile bool g_trace_faults;
void (*runtime_fault_context)(const char *line);
// Called only after Linux signal disposition is fatal, never for a handled trap.
void (*runtime_guest_terminated)(int tid, int signal, int exit_code);
// Per-process output rings never alter writes or use guest descriptors. Raw
// bytes remain in bounded memory; the iOS consumer filters/redacts on export.
void (*runtime_guest_output)(int tid, int tgid, int fd, const char *text);
struct output_record { int tgid; uint64_t start, sequence; unsigned next[2], lengths[2]; char lines[2][8][384]; char exit_request[384]; unsigned mapping_next; char mapping_failures[4][640]; };
static struct output_record outputs[64];
static lock_t output_lock=LOCK_INITIALIZER;
static uint64_t output_sequence;
static struct output_record *output_record(void) {
    uint64_t start=current->group->leader->start_ticks;
    struct output_record *old=&outputs[0];
    for(unsigned i=0;i<64;i++) {
        if(outputs[i].tgid==current->tgid && outputs[i].start==start)return &outputs[i];
        if(outputs[i].sequence<old->sequence)old=&outputs[i];
    }
    *old=(struct output_record){.tgid=current->tgid,.start=start};return old;
}
void runtime_capture_output(int fd,const void *buffer,size_t size) {
    if((fd!=1&&fd!=2)||!size||!current)return;
    const unsigned char *bytes=buffer;unsigned channel=fd-1;
    // A single huge write cannot monopolize the ring or the lock.
    if(size>4096){bytes+=size-4096;size=4096;}
    lock(&output_lock);struct output_record *r=output_record();r->sequence=++output_sequence;
    for(size_t i=0;i<size;i++) {
        unsigned slot=r->next[channel]%8, length=r->lengths[channel];
        if(bytes[i]=='\n') {r->lines[channel][slot][length]=0;r->next[channel]++;r->lengths[channel]=0;r->lines[channel][r->next[channel]%8][0]=0;continue;}
        if(length<383 && (bytes[i]=='\t'||bytes[i]>=32)) {r->lines[channel][slot][length++]=bytes[i];r->lines[channel][slot][length]=0;r->lengths[channel]=length;}
    }
    unlock(&output_lock);
}
static void report_guest_output(void) {
    if(!runtime_guest_output)return;
    struct output_record copy;
    lock(&output_lock);copy=*output_record();unlock(&output_lock);
    if (runtime_fault_context) {
        unsigned total = copy.mapping_next < 4 ? copy.mapping_next : 4;
        for (unsigned i = copy.mapping_next - total; i < copy.mapping_next; i++)
            runtime_fault_context(copy.mapping_failures[i % 4]);
    }
    if(copy.exit_request[0] && runtime_fault_context)runtime_fault_context(copy.exit_request);
    for(unsigned c=0;c<2;c++) {
        unsigned count=copy.next[c]<7?copy.next[c]:7;
        for(unsigned i=copy.next[c]-count;i<=copy.next[c];i++) {
            const char *line=copy.lines[c][i%8];if(*line)runtime_guest_output(current->pid,current->tgid,c+1,line);
        }
    }
}
struct recent_guest_call { uint64_t pc, args[6]; int64_t result; unsigned number; char failed_path[160]; };
static _Thread_local struct recent_guest_call recent_calls[16];
static _Thread_local unsigned recent_count;
static void remember_guest_call(unsigned number, uint64_t pc, const uint64_t args[6], int64_t result) {
    struct recent_guest_call *record = &recent_calls[recent_count++ % 16];
    *record = (struct recent_guest_call){.number=number, .pc=pc, .result=result};
    memcpy(record->args, args, sizeof(record->args));
#ifdef GUEST_ARM64
    // Preserve the path before later calls reuse the guest stack. Only failed
    // openat/faccessat paths are retained; no read/write payloads are captured.
    if (result < 0 && (number == 56 || number == 48 || number == 78)) {
        if (user_read_string(args[1], record->failed_path, sizeof(record->failed_path)))
            record->failed_path[0] = '\0';
    }
#endif
}
static void fault_context(const char *format, ...) {
    char line[640]; va_list args; va_start(args, format);
    vsnprintf(line, sizeof(line), format, args); va_end(args);
    printk("%s\n", line);
    if (runtime_fault_context) runtime_fault_context(line);
}
void runtime_record_mapping_failure(const char *line) {
    lock(&output_lock);
    struct output_record *r = output_record();
    r->sequence = ++output_sequence;
    snprintf(r->mapping_failures[r->mapping_next++ % 4], 640, "%s", line);
    unlock(&output_lock);
    fault_context("%s", line);
}
static void read_process_role(char type[80], char subtype[160]) {
    char args[4096];
    strcpy(type, "unspecified"); strcpy(subtype, "unspecified");
    if (!current->mm || current->mm->argv_end < current->mm->argv_start) return;
    uint64_t size = current->mm->argv_end - current->mm->argv_start;
    if (size > sizeof args) size = sizeof args;
    if (size && !user_read(current->mm->argv_start, args, size)) {
        for (size_t i = 0; i < size;) {
            size_t n = strnlen(args + i, size - i);
            if (n == size - i) break;
            const char *value = NULL; char *out = NULL; size_t cap = 0;
            if (!strncmp(args + i, "--type=", 7)) { value = args + i + 7; out = type; cap = 80; }
            if (!strncmp(args + i, "--utility-sub-type=", 19)) { value = args + i + 19; out = subtype; cap = 160; }
            if (value) {
                size_t j = 0;
                while (value[j] && j + 1 < cap && ((value[j] >= 'a' && value[j] <= 'z') ||
                        (value[j] >= 'A' && value[j] <= 'Z') || (value[j] >= '0' && value[j] <= '9') ||
                        value[j] == '.' || value[j] == '_' || value[j] == '-')) { out[j] = value[j]; j++; }
                out[j] = 0;
            }
            i += n + 1;
        }
    }
}
static void fault_process_role(void) {
    char type[80], subtype[160]; read_process_role(type, subtype);
    // Only role tokens are exported, never URLs or arbitrary argv contents.
    lock(&pids_lock);
    int parent = task_parent_pid(current);
    unlock(&pids_lock);
    fault_context("PROCESS_ROLE tid=%d tgid=%d ppid=%d type=%s utility_subtype=%s", current->pid,
            current->tgid, parent, type, subtype);
}
struct process_event { int tgid, subject, parent; char line[384]; };
static struct process_event process_events[64];
static unsigned process_sequence;
static lock_t process_event_lock=LOCK_INITIALIZER;
void runtime_process_event(const char *kind,int subject,int status) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    struct process_event event={.tgid=current->tgid,.subject=subject};
    snprintf(event.line,sizeof event.line,"PROCESS_EVENT tid=%d tgid=%d subject=%d kind=%s status=%d mono_ms=%llu pc=%#llx lr=%#llx",current->pid,current->tgid,subject,kind,status,(unsigned long long)now.tv_sec*1000+now.tv_nsec/1000000,(unsigned long long)current->cpu.pc,(unsigned long long)current->cpu.regs[30]);
    bool leader_event = current->pid == current->tgid &&
            (!strcmp(kind, "exec_committed") || !strcmp(kind, "thread_exit_status"));
    if (leader_event) {
        lock(&pids_lock);
        event.parent = task_parent_pid(current);
        unlock(&pids_lock);
        char type[80], subtype[160]; read_process_role(type, subtype);
        size_t used = strlen(event.line);
        snprintf(event.line + used, sizeof event.line - used, " ppid=%d type=%s utility_subtype=%s", event.parent, type, subtype);
    }
    lock(&process_event_lock);process_events[process_sequence++%64]=event;unlock(&process_event_lock);
    if (leader_event) fault_context("%s", event.line);
}
void runtime_output_exec_reset(void) {
    lock(&output_lock);struct output_record *r=output_record();int tgid=r->tgid;uint64_t start=r->start;*r=(struct output_record){.tgid=tgid,.start=start,.sequence=++output_sequence};unlock(&output_lock);
    recent_count=0;
}
void runtime_record_exit(unsigned syscall,unsigned status) {
    uint64_t args[6]={status,0,0,0,0,0};
    bool guest=current->cpu.regs[8]==syscall && current->cpu.pc==current->syscall_restart_pc;
    remember_guest_call(syscall,current->cpu.pc,args,0);
    lock(&output_lock);struct output_record *record=output_record();
    if(syscall==94 || !record->exit_request[0])snprintf(record->exit_request,sizeof record->exit_request,"PROCESS_EVENT tid=%d tgid=%d kind=termination_request name=%s status_raw=%u status_linux=%u origin=%s pc=%#llx lr=%#llx",current->pid,current->tgid,syscall==94?"exit_group":"exit",status,status&255,guest?"guest_syscall":"runtime_call",(unsigned long long)current->cpu.pc,(unsigned long long)current->cpu.regs[30]);
    unlock(&output_lock);
    runtime_process_event(guest?(syscall==94?"guest_exit_group_request":"guest_exit_request"):"runtime_exit_request",current->tgid,status);
}
static void report_process_events(void) {
    struct process_event copy[64];unsigned n;
    lock(&process_event_lock);memcpy(copy,process_events,sizeof copy);n=process_sequence;unlock(&process_event_lock);
    unsigned total=n<64?n:64,matched=0;
    for(unsigned i=n-total;i<n && matched<16;i++) {
        struct process_event *event=&copy[i%64];
        if(event->tgid==current->tgid || event->subject==current->tgid || event->parent==current->tgid){fault_context("%s",event->line);matched++;}
    }
}
static const char *call_name(unsigned n) {
    switch(n){case 46:return "ftruncate";case 113:return "clock_gettime";case 279:return "memfd_create";case 22:return "epoll_pwait";case 25:return "fcntl";case 56:return "openat";case 57:return "close";case 63:return "read";case 64:return "write";case 66:return "writev";case 73:return "ppoll";case 78:return "readlinkat";case 93:return "exit";case 94:return "exit_group";case 95:return "waitid";case 98:return "futex";case 138:return "rt_sigqueueinfo";case 240:return "rt_tgsigqueueinfo";case 198:return "socket";case 208:return "sendto";case 209:return "recvfrom";case 211:return "sendmsg";case 212:return "recvmsg";case 220:return "clone";case 221:return "execve";case 222:return "mmap";default:return "aarch64_syscall";}
}
static const char *error_name(int64_t result) {
    switch(result){case 0:return "OK";case -1:return "EPERM";case -2:return "ENOENT";case -4:return "EINTR";case -9:return "EBADF";case -11:return "EAGAIN";case -12:return "ENOMEM";case -13:return "EACCES";case -14:return "EFAULT";case -22:return "EINVAL";case -23:return "ENFILE";case -24:return "EMFILE";case -38:return "ENOSYS";case -88:return "ENOTSOCK";case -92:return "ENOPROTOOPT";case -110:return "ETIMEDOUT";default:return result<0?"other_Linux_errno":"OK";}
}
// Only exact, nonzero ELF function ranges are labeled. A nearest exported
// symbol in a stripped Chromium binary can be megabytes away and is misleading.
struct crash_section { uint32_t name,type;uint64_t flags,addr,offset,size;uint32_t link,info;uint64_t align,entsize; };
static void fault_symbol(struct fd *fd,uint64_t offset,const char *label) {
    if(!fd || !fd->ops->pread)return;
    struct elf_header64 h;
    if(fd->ops->pread(fd,&h,sizeof h,0)!=sizeof h || memcmp(&h.magic,ELF_MAGIC,4) || h.bitness!=2 || h.endian!=1 || h.machine!=183 || h.shent_size!=sizeof(struct crash_section) || h.shent_count>512 || h.phent_count>128 || h.phent_size!=sizeof(struct prg_header64) || h.secthead_off>INT64_MAX-512*sizeof(struct crash_section) || h.prghead_off>INT64_MAX-128*sizeof(struct prg_header64))return;
    uint64_t vaddr=0;bool mapped=false;
    for(unsigned i=0;i<h.phent_count;i++) {
        struct prg_header64 p;if(fd->ops->pread(fd,&p,sizeof p,h.prghead_off+i*sizeof p)!=sizeof p)return;
        if(p.type==PT_LOAD && offset>=p.offset && offset-p.offset<p.filesize && p.vaddr<=UINT64_MAX-(offset-p.offset)){vaddr=p.vaddr+offset-p.offset;mapped=true;break;}
    }
    if(!mapped)return;
    for(unsigned i=0;i<h.shent_count;i++) {
        struct crash_section section;
        if(fd->ops->pread(fd,&section,sizeof section,h.secthead_off+i*sizeof section)!=sizeof section)return;
        if((section.type!=2 && section.type!=11)||section.entsize!=sizeof(struct elf_sym64)||section.size/section.entsize>32768||section.link>=h.shent_count||section.offset>INT64_MAX-section.size)continue;
        struct crash_section strings;
        if(fd->ops->pread(fd,&strings,sizeof strings,h.secthead_off+section.link*sizeof strings)!=sizeof strings || strings.offset>INT64_MAX-strings.size)continue;
        for(uint64_t j=0;j<section.size/section.entsize;j++) {
            struct elf_sym64 symbol;
            if(fd->ops->pread(fd,&symbol,sizeof symbol,section.offset+j*sizeof symbol)!=sizeof symbol)break;
            if((symbol.info&15)!=2 || !symbol.size || !symbol.shndx || vaddr<symbol.value || vaddr-symbol.value>=symbol.size || symbol.name>=strings.size)continue;
            char name[128]={0};size_t count=strings.size-symbol.name;if(count>=sizeof name)count=sizeof name-1;
            if(fd->ops->pread(fd,name,count,strings.offset+symbol.name)<1)return;
            for(unsigned k=0;k<sizeof name-1 && name[k];k++)if((unsigned char)name[k]<32 || (unsigned char)name[k]>=127)name[k]='?';
            fault_context("FAULT_SYMBOL tid=%d frame=%s symbol=%s+%#llx exact_range=1",current->pid,label,name,(unsigned long long)(vaddr-symbol.value));return;
        }
    }
}
static void fault_mapping(uint64_t address, const char *label) {
    char path[MAX_PATH] = ""; uint64_t offset = 0; unsigned flags = 0; bool found = false; struct fd *backing=NULL;
    read_wrlock(&current->mem->lock);
    struct pt_entry *pt = mem_pt(current->mem, PAGE(address));
    if (pt && pt->data) {
        found = true; flags = pt->flags;
        offset = pt->data->file_offset + pt->offset + PGOFFSET(address);
        if (pt->data->fd) { backing=fd_retain(pt->data->fd); generic_getpath(backing, path); }
        else if (pt->data->name) snprintf(path, sizeof(path), "%s", pt->data->name);
    }
    read_wrunlock(&current->mem->lock);
    fault_context("FAULT_MAP tid=%d %s=%#llx mapped=%d flags=%#x file-offset=%#llx path=%.380s", current->pid, label, (unsigned long long)address, found, flags, (unsigned long long)offset, path);
    if(backing) { fault_symbol(backing,offset,label); fd_close(backing); }
}
struct fault_fd_census { unsigned owners, origins, unix_pending; };
static void report_census_line(const char *line,void *context) {
    struct fault_fd_census *counts=context;
    if(!strncmp(line,"FD_OWNER ",9) && counts->owners++>=24)return;
    if(!strncmp(line,"FD_ORIGIN ",10) && counts->origins++>=24)return;
    if(!strncmp(line,"FD_UNIX_PENDING ",16) && counts->unix_pending++>=24)return;
    fault_context("%s",line);
}
// ---- Compatibility census -------------------------------------------------
// Aggregates what guests asked for and did not get (missing/stub syscalls,
// ioctls answered ENOTTY, fatal guest faults with the faulting instruction),
// keyed by process name, so application tests and on-device users can see
// which Linux features an app needs (/proc/ish/compat). Bounded; never alters
// the result the guest sees.
struct compat_event { char comm[16]; uint8_t kind; uint32_t code, aux, count; uint64_t first_ms; };
static struct compat_event compat_events[512];
static unsigned compat_used, compat_dropped;
static pthread_mutex_t compat_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const compat_kind_names[] = {"none", "missing_syscall", "stub_syscall", "ioctl_enotty", "fault"};
void compat_record(int kind, uint32_t code, uint32_t aux) {
    const char *comm = current ? current->comm : "host";
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&compat_lock);
    unsigned i = 0;
    for (; i < compat_used; i++) {
        struct compat_event *e = &compat_events[i];
        if (e->kind == kind && e->code == code && e->aux == aux && !strncmp(e->comm, comm, sizeof e->comm)) { e->count++; break; }
    }
    if (i == compat_used) {
        if (compat_used < sizeof(compat_events) / sizeof(compat_events[0])) {
            struct compat_event *e = &compat_events[compat_used++];
            strncpy(e->comm, comm, sizeof e->comm - 1); e->comm[sizeof e->comm - 1] = 0;
            e->kind = (uint8_t) kind; e->code = code; e->aux = aux; e->count = 1;
            e->first_ms = (uint64_t) now.tv_sec * 1000 + (uint64_t) now.tv_nsec / 1000000;
        } else compat_dropped++;
    }
    pthread_mutex_unlock(&compat_lock);
}
void compat_census(void (*emit)(const char *, void *), void *context) {
    pthread_mutex_lock(&compat_lock);
    for (unsigned i = 0; i < compat_used; i++) {
        struct compat_event *e = &compat_events[i]; char line[160];
        snprintf(line, sizeof line, "COMPAT comm=%s kind=%s code=%#x aux=%#x count=%u first_ms=%llu", e->comm,
            compat_kind_names[e->kind < 5 ? e->kind : 0], e->code, e->aux, e->count, (unsigned long long) e->first_ms);
        emit(line, context);
    }
    char line[64]; snprintf(line, sizeof line, "COMPAT_SUMMARY entries=%u dropped=%u", compat_used, compat_dropped);
    emit(line, context);
    pthread_mutex_unlock(&compat_lock);
}

// Repeated identical faults (same program, PC and signal — e.g. a respawned
// shell crashing in a loop, GitHub issue #3) keep the first reports complete
// and then shrink to counts, so diagnostics cannot flood the log or the disk.
#define FAULT_REPEAT_SLOTS 32
#define FAULT_FULL_REPORTS 2     // complete context for the first occurrences
#define FAULT_LINE_REPORTS 8     // then one GUEST_FAULT line each, up to this count
#define FAULT_SUMMARY_EVERY 64   // then a counted summary line every N occurrences
static struct { char comm[16]; uint64_t pc; int signal; unsigned count; } fault_repeats[FAULT_REPEAT_SLOTS];
static unsigned fault_repeat_next;
static pthread_mutex_t fault_repeat_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned fault_occurrence(int signal) {
    pthread_mutex_lock(&fault_repeat_lock);
    unsigned count = 0;
    for (unsigned i = 0; i < FAULT_REPEAT_SLOTS && !count; i++)
        if (fault_repeats[i].count && fault_repeats[i].pc == current->cpu.pc && fault_repeats[i].signal == signal &&
                !strncmp(fault_repeats[i].comm, current->comm, sizeof fault_repeats[i].comm))
            count = ++fault_repeats[i].count;
    if (!count) {
        unsigned i = fault_repeat_next++ % FAULT_REPEAT_SLOTS;
        strncpy(fault_repeats[i].comm, current->comm, sizeof fault_repeats[i].comm);
        fault_repeats[i].pc = current->cpu.pc; fault_repeats[i].signal = signal; fault_repeats[i].count = count = 1;
    }
    pthread_mutex_unlock(&fault_repeat_lock);
    return count;
}

void runtime_report_guest_fault(int signal, uint64_t address, uint32_t instruction) {
    if (!instruction) { uint32_t word; if (!user_get(current->cpu.pc,word)) instruction=word; }
    int ppid = 0;
    lock(&pids_lock);
    ppid = task_parent_pid(current);
    unlock(&pids_lock);
    unsigned occurrence = fault_occurrence(signal);
    compat_record(COMPAT_FAULT, signal == 4 ? instruction : 0, (uint32_t) signal);
    if (occurrence > FAULT_LINE_REPORTS) {
        if (occurrence % FAULT_SUMMARY_EVERY == 0)
            printk("GUEST_FAULT_REPEATED: comm=%.16s signal=%d pc=%#llx occurrences=%u (details were reported for the first %d)\n",
                current->comm, signal, (unsigned long long)current->cpu.pc, occurrence, FAULT_FULL_REPORTS);
        return;
    }
    printk("GUEST_FAULT: pid=%d ppid=%d comm=%.16s signal=%d pc=%#llx sp=%#llx address=%#llx instruction=%#x occurrence=%u\n",
        current->pid, ppid, current->comm, signal, (unsigned long long)current->cpu.pc,
        (unsigned long long)current->cpu.sp, (unsigned long long)address, instruction, occurrence);
    if (occurrence > FAULT_FULL_REPORTS) return;
    if (g_trace_faults) {
        for (unsigned i = 0; i < 31; i++) {
            printk("GUEST_REGISTER: pid=%d x%u=%#llx\n", current->pid, i,
                (unsigned long long) current->cpu.regs[i]);
        }
    }
    if (runtime_guest_fault) runtime_guest_fault(current->pid, ppid, current->comm,
        current->cpu.pc, current->cpu.sp, signal, address, instruction);
    // Bounded persistent context, even without verbose tracing. Never change
    // guest state, suppress signals, or fabricate faulting data.
    char executable[MAX_PATH] = "";
    if (current->mm && current->mm->exefile) generic_getpath(current->mm->exefile, executable);
    fault_context("FAULT_ID tid=%d tgid=%d ppid=%d executable=%.380s nzcv=%#x",current->pid,current->tgid,ppid,executable,current->cpu.nzcv);
    fault_mapping(current->cpu.pc, "PC"); fault_mapping(current->cpu.regs[30], "LR");
    // Code mappings alone cannot distinguish an unmapped pointer from a
    // protection fault. Inspect page metadata only: never dereference a bad
    // pointer or dump application buffers while reporting a guest failure.
    if ((signal == SIGSEGV_ || signal == SIGBUS_) && address)
        fault_mapping(address, "ADDRESS");
    if (g_trace_faults) {
        for (unsigned i = 0; i < 4; i++) {
            char label[16]; snprintf(label, sizeof label, "X%u_MAPPING", i);
            fault_mapping(current->cpu.regs[i], label);
        }
    }
    for (int i=-4; i<=4; i++) {
        uint64_t pc=current->cpu.pc+i*4; uint32_t word;
        if (!user_get(pc, word)) fault_context("FAULT_CODE pc=%#llx instruction=%#x",(unsigned long long)pc,word);
    }
    for (unsigned i = 0; i < 28; i += 4) {
        fault_context("FAULT_REG x%u=%#llx x%u=%#llx x%u=%#llx x%u=%#llx",
            i, (unsigned long long)current->cpu.regs[i],
            i+1, (unsigned long long)current->cpu.regs[i+1],
            i+2, (unsigned long long)current->cpu.regs[i+2],
            i+3, (unsigned long long)current->cpu.regs[i+3]);
    }
    fault_context("FAULT_REG x28=%#llx x29=%#llx x30=%#llx sp=%#llx",
        (unsigned long long)current->cpu.regs[28], (unsigned long long)current->cpu.regs[29],
        (unsigned long long)current->cpu.regs[30], (unsigned long long)current->cpu.sp);
    unsigned count=recent_count<16?recent_count:16;
    for (unsigned i=recent_count-count; i<recent_count; i++) {
        struct recent_guest_call *r=&recent_calls[i%16];
        fault_context("FAULT_CALL tid=%d pc=%#llx nr=%u args=%llx,%llx,%llx,%llx,%llx,%llx result=%lld name=%s errno=%s",current->pid,(unsigned long long)r->pc,r->number,(unsigned long long)r->args[0],(unsigned long long)r->args[1],(unsigned long long)r->args[2],(unsigned long long)r->args[3],(unsigned long long)r->args[4],(unsigned long long)r->args[5],(long long)r->result,call_name(r->number),error_name(r->result));
        if (r->failed_path[0]) fault_context("FAULT_PATH tid=%d nr=%u result=%lld path=%.159s", current->pid, r->number, (long long)r->result, r->failed_path);
    }
    fault_process_role();
    report_process_events();
    report_guest_output();
    fd_diagnostic_snapshot();
    if(runtime_fault_context) {
        struct fault_fd_census counts={0};fd_global_census(report_census_line,&counts);
        fault_context("FD_CENSUS_LIMIT owners=%u origins=%u retained_per_kind=24",counts.owners,counts.origins);
    }
    uint64_t fp=current->cpu.regs[29];
    for (unsigned i=0;i<8;i++) {
        uint64_t next,lr;
        if ((fp&15)||user_get(fp,next)||user_get(fp+8,lr)) break;
        fault_mapping(lr,"FRAME");
        if(next<=fp || next-fp>1024*1024) break;
        fp=next;
    }
    fault_context("FAULT_END tid=%d", current->pid);
}

dword_t syscall_stub(void) {
    return _ENOSYS;
}
// While identical, this version of the stub doesn't log below. Use this for
// syscalls that are optional (i.e. fallback on something else) but called
// frequently.
dword_t syscall_silent_stub(void) {
    return _ENOSYS;
}
dword_t syscall_success_stub(void) {
    return 0;
}

// Syscall table is defined in arch-specific files (kernel/arch/*/calls.c)
extern syscall_t syscall_table[];
extern size_t syscall_table_size;

void dump_stack(int lines);
void dump_maps(void);

// Fast path syscall handlers (forward declarations)
static inline int fast_fstat64(struct cpu_state *cpu);
static inline int fast_read(struct cpu_state *cpu);
static inline int fast_write(struct cpu_state *cpu);

// Diagnostic: JIT crash info from signal handler
__thread volatile uint64_t jit_last_host_fault = 0;
__thread volatile uint64_t jit_last_x7 = 0;
__thread volatile uint64_t jit_last_x10 = 0;
__thread volatile int jit_crash_count = 0;
volatile bool g_trace_faults = false;
volatile bool g_trace_syscalls = false;

// Opt-in diagnostic (ISH_IPC_TRACE=1): lock-free ring of the most recent
// IPC/wait syscalls, numeric arguments only (no payloads, paths or strings).
// Blocking calls also record entry so a call that never returns is visible.
// Reading /proc/ish/ipctrace writes the ring to ISH_IPC_TRACE_FILE on the host.
struct ipc_trace_record { uint64_t ns; int32_t tid, tgid; uint16_t nr, phase; uint64_t a[4]; int64_t result; };
#define IPC_TRACE_RECORDS (1u << 21)
static struct ipc_trace_record *ipc_trace_ring;
static atomic_ullong ipc_trace_head;
static void __attribute__((constructor)) ipc_trace_init(void) {
    const char *enabled = getenv("ISH_IPC_TRACE");
    if (enabled && !strcmp(enabled, "1") && getenv("ISH_IPC_TRACE_FILE"))
        ipc_trace_ring = calloc(IPC_TRACE_RECORDS, sizeof(*ipc_trace_ring));
}
static bool ipc_trace_selected(unsigned nr) {
    switch (nr) {
        case 19: case 21: case 22: case 57: case 59: case 63: case 64: case 73:
        case 93: case 94: case 98: case 199: case 203: case 206: case 207:
        case 211: case 212: case 220: case 221: case 260: case 435:
        // File and lock activity (fd/flags/offsets only): openat, fcntl, flock,
        // pread64, pwrite64, fsync, fdatasync.
        case 56: case 25: case 32: case 67: case 68: case 82: case 83: return true;
    }
    return false;
}
static bool ipc_trace_blocking(unsigned nr) {
    return nr == 22 || nr == 63 || nr == 73 || nr == 98 || nr == 207 || nr == 212 || nr == 260 ||
        nr == 25 || nr == 32 || nr == 67 || nr == 68 || nr == 82 || nr == 83;
}
static void ipc_trace_record(unsigned nr, unsigned phase, const uint64_t *args, int64_t result) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long long slot = atomic_fetch_add_explicit(&ipc_trace_head, 1, memory_order_relaxed);
    struct ipc_trace_record *r = &ipc_trace_ring[slot % IPC_TRACE_RECORDS];
    *r = (struct ipc_trace_record){(uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec,
        current->pid, current->tgid, (uint16_t)nr, (uint16_t)phase,
        {args[0], args[1], args[2], args[3]}, result};
}
int ipc_trace_dump(char *status, size_t size) {
    if (!ipc_trace_ring) { snprintf(status, size, "IPC_TRACE disabled\n"); return 0; }
    // procfs invokes show() several times for one read; dump at most once per 2 s.
    static atomic_ullong last_dump_ns;
    struct timespec started; clock_gettime(CLOCK_MONOTONIC, &started);
    unsigned long long started_ns = (unsigned long long)started.tv_sec * 1000000000ull + started.tv_nsec;
    unsigned long long previous = atomic_load(&last_dump_ns);
    if (previous && started_ns - previous < 2000000000ull) { snprintf(status, size, "IPC_TRACE recent_dump_skipped\n"); return 0; }
    if (!atomic_compare_exchange_strong(&last_dump_ns, &previous, started_ns)) { snprintf(status, size, "IPC_TRACE concurrent_dump_skipped\n"); return 0; }
    const char *path = getenv("ISH_IPC_TRACE_FILE");
    FILE *out = fopen(path, "a");
    if (!out) { snprintf(status, size, "IPC_TRACE open_failed errno=%d\n", errno); return 0; }
    // Incremental: each dump continues where the previous one ended (or the
    // oldest retained record if the ring wrapped since then).
    static unsigned long long dumped_head;
    unsigned long long head = atomic_load(&ipc_trace_head);
    unsigned long long first = head > IPC_TRACE_RECORDS ? head - IPC_TRACE_RECORDS : 0;
    if (first < dumped_head) first = dumped_head;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(out, "IPC_TRACE_DUMP now_ns=%llu head=%llu first=%llu\n",
        (unsigned long long)now.tv_sec * 1000000000ull + now.tv_nsec, head, first);
    // Records racing with this dump may be torn; the analysis treats each line as best effort.
    for (unsigned long long i = first; i < head; i++) {
        struct ipc_trace_record r = ipc_trace_ring[i % IPC_TRACE_RECORDS];
        fprintf(out, "%llu %d %d %u %c %llx %llx %llx %llx %lld\n", (unsigned long long)r.ns, r.tid, r.tgid,
            r.nr, r.phase ? 'X' : 'E', (unsigned long long)r.a[0], (unsigned long long)r.a[1],
            (unsigned long long)r.a[2], (unsigned long long)r.a[3], (long long)r.result);
    }
    fclose(out);
    dumped_head = head;
    snprintf(status, size, "IPC_TRACE dumped=%llu\n", head - first);
    return 0;
}

void handle_interrupt(int interrupt) {
    static atomic_uint unsupported_counts[512];
    struct cpu_state *cpu = &current->cpu;
    if (interrupt == INT_SYSCALL) {
        // ARM64: syscall number in x8, args in x0-x5, return in x0
        unsigned syscall_num = cpu->regs[8];
        // PTRACE_SYSCALL entry stop. The tracer may change the number (x8) or
        // arguments; -1 skips the call and x0 is the result, as on arm64 Linux.
        bool ptrace_skip = false;
        if (current->ptrace.syscall_trace) {
            current->ptrace.in_syscall = true;
            ptrace_syscall_stop();
            syscall_num = cpu->regs[8];
            ptrace_skip = (int32_t) syscall_num == -1;
        }
        uint64_t observe_args[6] = {0}; struct timespec observe_start = {0};
        bool observe = runtime_syscall_result_observer != NULL;
        bool timed = observe && atomic_load(&runtime_syscall_timing_enabled);
        if (observe) memcpy(observe_args, cpu->regs, sizeof observe_args);
        if (timed) clock_gettime(CLOCK_MONOTONIC, &observe_start);
        if (runtime_syscall_observer) runtime_syscall_observer(current->pid, cpu->pc, cpu->sp, syscall_num);
        // Snapshot for SA_RESTART: x0 (first arg) is about to be clobbered by
        // the return value; save it and the number so an interrupted syscall
        // can be re-executed if a SA_RESTART signal is delivered.
        current->syscall_restart_num = syscall_num;
        current->syscall_restart_arg0 = cpu->regs[0];
        // The svc gadget already advanced cpu.pc to the instruction after the
        // svc. Record it so the SA_RESTART rewind can verify PC is still exactly
        // here (i.e. we really are delivering a signal for THIS interrupted
        // syscall) before doing pc -= 4.
        current->syscall_restart_pc = cpu->pc;
        current->syscall_restartable = false;
        current->syscall_restart_always = false;

        // === FAST PATH: Hot syscalls ===
        int fast_result = -1;  // -1 means fast path not taken
        bool fast_path_taken = false;

        if (ptrace_skip) {
            fast_result = (int) (int64_t) cpu->regs[0];
            fast_path_taken = true;
        }
        // Fast path 1: fstat64 (syscall 80) - most common during Python import
        else if (syscall_num == 80) {
            fast_result = fast_fstat64(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }
        // Fast path 2: read (syscall 63) - small buffer reads
        else if (syscall_num == 63) {
            fast_result = fast_read(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }
        // Fast path 3: write (syscall 64) - small buffer writes
        else if (syscall_num == 64) {
            fast_result = fast_write(cpu);
            if (fast_result != -1)
                fast_path_taken = true;
        }

        if (fast_path_taken) {
            // Fast path succeeded, return immediately
            // Fast paths return int (small values), safe to use directly
            STRACE("%d call %-3d (fast) = 0x%x\n", current->pid, syscall_num, fast_result);
            int32_t signed_result = (int32_t)fast_result;
            if (signed_result >= -4095 && signed_result < 0) {
                cpu->regs[0] = (uint64_t)(int64_t)signed_result;
            } else {
                cpu->regs[0] = (uint64_t)(uint32_t)fast_result;
            }
        } else {
            // === SLOW PATH: Full syscall ===
            if (syscall_num >= syscall_table_size || syscall_table[syscall_num] == NULL) {
                printk("%d(%s) missing syscall %d\n", current->pid, current->comm, syscall_num);
                compat_record(COMPAT_MISSING_SYSCALL, (uint32_t) syscall_num, 0);
                cpu->regs[0] = (uint64_t)(int64_t)(int32_t)_ENOSYS;
            } else {
                if (syscall_table[syscall_num] == (syscall_t) syscall_stub) {
                    unsigned occurrence=syscall_num<512?atomic_fetch_add(&unsupported_counts[syscall_num],1)+1:1;
                    compat_record(COMPAT_STUB_SYSCALL, (uint32_t) syscall_num, 0);
                    if (g_trace_syscalls || !(occurrence & (occurrence-1))) printk("%d(%s) stub syscall %d occurrences=%u (ENOSYS; further repeats rate limited)\n", current->pid, current->comm, syscall_num, occurrence);
                }
                STRACE("%d call %-3d ", current->pid, syscall_num);
                // env-gated full syscall trace for golden-diff vs native strace.
                // ISH_SYSTRACE=1 logs every slow-path syscall with timestamp,
                // pid, number, the 6 args, and the return value.
                uint64_t diagnostic_pc = cpu->pc;
                uint64_t diagnostic_args[6];
                memcpy(diagnostic_args, cpu->regs, sizeof diagnostic_args);
                // Completion-only tracing hides a syscall that never returns.
                // Bound this numeric-only blocking-operation trace per host
                // thread; no guest strings, buffers or keyboard text are read.
                static _Thread_local unsigned blocking_records;
                bool trace_block = g_trace_syscalls && blocking_records < 1024 &&
                    (syscall_num == 22 || syscall_num == 63 || syscall_num == 72 ||
                     syscall_num == 73 || syscall_num == 98 || syscall_num == 202 ||
                     syscall_num == 203 || syscall_num == 207 || syscall_num == 212 ||
                     syscall_num == 260);
                unsigned blocking_sequence = trace_block ? ++blocking_records : 0;
                bool ipc_traced = ipc_trace_ring && ipc_trace_selected(syscall_num);
                if (ipc_traced && ipc_trace_blocking(syscall_num))
                    ipc_trace_record(syscall_num, 0, diagnostic_args, 0);
                if (trace_block)
                    printk("ABI_BLOCK_ENTER: tid=%d sequence=%u pc=%#llx nr=%u args=%llx,%llx,%llx,%llx,%llx,%llx\n",
                        current->pid, blocking_sequence, (unsigned long long)diagnostic_pc, syscall_num,
                        (unsigned long long)diagnostic_args[0], (unsigned long long)diagnostic_args[1],
                        (unsigned long long)diagnostic_args[2], (unsigned long long)diagnostic_args[3],
                        (unsigned long long)diagnostic_args[4], (unsigned long long)diagnostic_args[5]);
                memcpy(current->current_syscall_args, diagnostic_args, sizeof diagnostic_args);
                current->current_syscall_pc = diagnostic_pc;
                atomic_store_explicit(&current->current_syscall, syscall_num + 1, memory_order_release);
                int64_t result = syscall_table[syscall_num](
                    cpu->regs[0], cpu->regs[1], cpu->regs[2],
                    cpu->regs[3], cpu->regs[4], cpu->regs[5]);
                atomic_store_explicit(&current->current_syscall, 0, memory_order_release);
                STRACE(" = 0x%llx\n", (unsigned long long)result);
                int64_t diagnostic_result = result;
                if ((uint64_t)result >= 0xfffff001ULL && (uint64_t)result <= 0xffffffffULL)
                    diagnostic_result = (int32_t)result;
                if (trace_block)
                    printk("ABI_BLOCK_EXIT: tid=%d sequence=%u nr=%u result=%lld\n",
                        current->pid, blocking_sequence, syscall_num, (long long)diagnostic_result);
                if (ipc_traced) ipc_trace_record(syscall_num, 1, diagnostic_args, diagnostic_result);
                remember_guest_call(syscall_num, diagnostic_pc, diagnostic_args, diagnostic_result);
                if (diagnostic_result == _EMFILE || diagnostic_result == _ENFILE)
                    fd_exhaustion_report(syscall_num, (int)diagnostic_result);
                if (syscall_num == 98 && diagnostic_result < 0 &&
                    diagnostic_result != _EAGAIN && diagnostic_result != _EINTR && diagnostic_result != _ETIMEDOUT) {
                    static atomic_uint futex_errors;
                    if (atomic_fetch_add_explicit(&futex_errors, 1, memory_order_relaxed) < 32)
                        fault_context("FUTEX_ERROR: tid=%d tgid=%d op=%#llx addr=%#llx value=%#llx timeout_or_count=%#llx addr2=%#llx val3=%#llx result=%lld",
                            current->pid,current->tgid,(unsigned long long)diagnostic_args[1],
                            (unsigned long long)diagnostic_args[0],(unsigned long long)diagnostic_args[2],
                            (unsigned long long)diagnostic_args[3],(unsigned long long)diagnostic_args[4],
                            (unsigned long long)diagnostic_args[5],(long long)diagnostic_result);
                }
                if (g_trace_syscalls && (diagnostic_result < 0 || syscall_num == 199 || syscall_num == 208 || syscall_num == 209 || syscall_num == 220 || syscall_num == 279 || ((syscall_num == 222 || syscall_num == 226) && diagnostic_args[1] >= 1024*1024) || syscall_num == 25)) {
                    int diagnostic_ppid = 0;
                    lock(&pids_lock); diagnostic_ppid=task_parent_pid(current); unlock(&pids_lock);
                    static atomic_uint records;
                    unsigned record = atomic_fetch_add(&records, 1);
                    if (record < 10000) printk("ABI_TRACE: pid=%d ppid=%d comm=%.16s pc=%#llx sp=%#llx n=%u args=%llx,%llx,%llx,%llx,%llx,%llx result=%lld\n",
                        current->pid, diagnostic_ppid, current->comm,
                        (unsigned long long)cpu->pc, (unsigned long long)cpu->sp, syscall_num,
                        (unsigned long long)diagnostic_args[0],(unsigned long long)diagnostic_args[1],
                        (unsigned long long)diagnostic_args[2],(unsigned long long)diagnostic_args[3],
                        (unsigned long long)diagnostic_args[4],(unsigned long long)diagnostic_args[5],(long long)result);
                    if (record == 10000) printk("ABI_TRACE: record limit reached\n");
                }
                // SA_RESTART: mark this syscall restartable if it was
                // interrupted (EINTR). receive_signals will rewind PC if the
                // delivered signal's handler has SA_RESTART. Restricted to
                // syscalls that re-execute correctly by simply re-running with
                // the same args (futex, poll/epoll, read/write, recv/send,
                // accept, connect, wait). Timer syscalls (nanosleep,
                // clock_nanosleep, ppoll/pselect with timeout) must NOT use
                // naive PC-rewind restart: it would restart the FULL duration
                // rather than the remaining time, so a `timeout N` wrapper's
                // sleep would never end. Those keep returning EINTR (userspace
                // handles their own restart with an adjusted timeout).
                if ((int32_t)(uint32_t)result == -EINTR) {
                    switch (syscall_num) {
                        case 98:  // futex
                        case 22:  // epoll_pwait
                        case 63:  // read
                        case 64:  // write
                            current->syscall_restartable = true;
                            break;
                        default:
                            current->syscall_restartable = false;
                    }
                }
                // sigreturn/rt_sigreturn restore the full CPU state from the
                // signal frame. Do NOT touch regs[0] — it was already restored
                // by restore_sigcontext. Any post-processing could corrupt the
                // restored x0 value (e.g., errno sign-extension).
                if (syscall_num == 139 /* rt_sigreturn */) {
                    // x0 already set by restore_sigcontext, skip writeback
                } else if (syscall_num == 62 /* lseek */) {
                    // sys_lseek64 already returns a full-width value (including
                    // sign-extended errors). Valid offsets 0xfffff001..0xffffffff
                    // must not be mistaken for zero-extended 32-bit errnos.
                    cpu->regs[0] = (uint64_t)result;
                } else {
                    // ARM64 Linux ABI: return value in x0. Errors are negative
                    // (-1 to -4095). Success values are 0 or positive (up to 48-bit
                    // addresses from mmap/brk).
                    // Most syscalls return dword_t (uint32_t). On ARM64 ABI, returning
                    // uint32_t sets w0 only; upper x0 is zero. Error codes like -EFAULT
                    // become 0x00000000FFFFFFF2 instead of 0xFFFFFFFFFFFFFFF2.
                    // Detect this: if low 32 bits are a valid errno (0xFFFFF001-0xFFFFFFFF)
                    // but upper 32 bits are 0, sign-extend the 32-bit value.
                    uint32_t low32 = (uint32_t)result;
                    if ((result >> 32) == 0 && (int32_t)low32 >= -4095 && (int32_t)low32 < 0) {
                        // 32-bit errno from dword_t-returning syscall — sign-extend
                        cpu->regs[0] = (uint64_t)(int64_t)(int32_t)low32;
                    } else {
                        cpu->regs[0] = (uint64_t)result;
                    }
                }
            }
        }
        // [T-ish-group-null-post-syscall-crash] The syscall we just ran may have
        // left this thread orphaned: a blocking syscall (futex/poll/wait) can
        // return right as another thread's do_exit_group() safety valve
        // force-releases us — it sets our `exiting` flag — and, once the group
        // leader is reaped, task_destroy() does memset(task, 0, ...) which
        // zeroes our (thread-local) `current` struct, so both `current->group`
        // and `current->exiting` read back as 0/NULL. The post-syscall
        // bookkeeping below dereferences current->group unconditionally; with a
        // zeroed group that computes &((tgroup*)0)->syscall_count == 0x4fc and
        // the atomic_fetch_add faults (the reported EXC_BAD_ACCESS at 0x4fc on a
        // backgrounded guest thread). task_run_current()'s top-of-loop guard
        // already handles this, but only on the NEXT iteration — we must not
        // touch group before we get back there.
        //
        // Bail the SAME way task_run_current() does for a destroyed/leaked task:
        // pthread_exit() straight out, letting task_run_current()'s
        // pthread_cleanup handler (task_run_tlb_cleanup) run — it honors
        // mm_release_deferred and frees the tlb. Do NOT call do_exit() here:
        //   * for the zeroed-struct case (group==NULL, exiting==0) do_exit()
        //     would fall through its `if (current->exiting)` fast-path and then
        //     dereference current->group->doing_group_exit — the same NULL crash
        //     one line over;
        //   * for the force-released case (exiting==1) do_exit() sets
        //     current=NULL before pthread_exit(), which makes task_run_tlb_cleanup
        //     skip the deferred mm_release and leak the guest address space.
        // Reading group_exit_code for a status is pointless anyway — do_exit()
        // ignores its argument once `exiting` is set — and unsafe, since the
        // group may already be freed.
        if (current->exiting || current->group == NULL) {
            pthread_exit(NULL);
        }
        // PTRACE_SYSCALL exit stop (x0 holds the result).
        if (current->ptrace.in_syscall) {
            ptrace_syscall_stop();
            current->ptrace.in_syscall = false;
        }
        if (observe && runtime_syscall_result_observer && syscall_num != 139) {
            uint64_t ns = 0;
            if (timed) { struct timespec end; clock_gettime(CLOCK_MONOTONIC, &end); ns = (end.tv_sec-observe_start.tv_sec)*1000000000ULL + end.tv_nsec-observe_start.tv_nsec; }
            runtime_syscall_result_observer(current->pid, current->tgid, syscall_num, observe_args, (int64_t)cpu->regs[0], ns);
        }
        // Update deadlock detection state.
        atomic_fetch_add(&current->group->syscall_count, 1);
        // Update last_unblocked_ns for non-blocking syscalls only.
        // Blocking calls (futex, epoll, ppoll, waitid, nanosleep) that
        // return and retry should NOT refresh the timestamp, as they
        // don't represent real forward progress.
        if (syscall_num != 22 && syscall_num != 73 && syscall_num != 95 &&
            syscall_num != 98 && syscall_num != 101) {
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            uint64_t now = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
            current->last_unblocked_ns = now;
            atomic_store_explicit(&current->group->last_progress_ns, now,
                                  memory_order_relaxed);
        }
        // Check for pending group exit. When do_exit_group sends SIGKILL to
        // all threads, a thread stuck in a retrying syscall (e.g., musl's
        // futex_wait loops on EINTR) would never return to the JIT dispatch
        // loop where receive_signals() handles SIGKILL. Catch it here.
        if (current->group->doing_group_exit)
            do_exit(current->group->group_exit_code);
    } else if (interrupt == INT_OOM) {
        // Do not disguise host allocation failure as a guest page fault: a
        // mapped code page would otherwise be retried forever. Kill only the
        // affected guest process group, as for a host OOM victim.
        do_exit_group(SIGKILL_);
    } else if (interrupt == INT_GPF) {
        struct timespec fault_begin, fault_end;
        clock_gettime(CLOCK_MONOTONIC, &fault_begin);
        __atomic_add_fetch(&current->diagnostic_faults, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&current->diagnostic_fault_address, cpu->segfault_addr, __ATOMIC_RELAXED);
        read_wrlock(&current->mem->lock);
        int fault_access = cpu->segfault_was_write ? MEM_WRITE : MEM_READ;
#ifdef GUEST_ARM64
        if (cpu->segfault_was_exec) fault_access = MEM_EXEC;
#endif
        void *ptr = mem_ptr(current->mem, cpu->segfault_addr, fault_access);
        read_wrunlock(&current->mem->lock);
        clock_gettime(CLOCK_MONOTONIC, &fault_end);
        uint64_t fault_ns = (fault_end.tv_sec - fault_begin.tv_sec) * 1000000000ULL +
            fault_end.tv_nsec - fault_begin.tv_nsec;
        __atomic_add_fetch(&current->diagnostic_fault_ns, fault_ns, __ATOMIC_RELAXED);
        if (ptr) __atomic_add_fetch(&current->diagnostic_resolved_faults, 1, __ATOMIC_RELAXED);
        // Retry only a real mapping fault resolved by mem_ptr (CoW, a
        // demand-zero reservation, or legitimate stack growth). An unmapped
        // address must deliver SIGSEGV; never fabricate neighboring heap data
        // or unwind application-specific frames to conceal guest corruption.
        if (ptr == NULL) {
            struct siginfo_ info = {
                .code = mem_segv_reason(current->mem, cpu->segfault_addr),
                .fault.addr = cpu->segfault_addr,
            };
            if (g_trace_faults) {
                dump_stack(8);
                dump_maps();
            }
            uint32_t fault_instruction = 0; if (user_get(cpu->pc, fault_instruction)) fault_instruction = 0;
            runtime_report_guest_fault(SIGSEGV_, cpu->segfault_addr, fault_instruction);
            deliver_signal(current, SIGSEGV_, info);
        }
    } else if (interrupt == INT_UNDEFINED) {
        uint32_t ill_insn = 0;
        {
            for (int i = 0; i < 4; i++) {
                uint8_t b;
                if (user_get(cpu->pc + i, b))
                    break;
                ill_insn |= (uint32_t)b << (i * 8);
            }
            printk("%d illegal instruction at 0x%llx: insn=0x%08x\n", current->pid, (unsigned long long)cpu->pc, ill_insn);
            if (runtime_instruction_failure)
                runtime_instruction_failure(current->pid, cpu->pc, ill_insn);
        }
        printk("\n");
        dump_stack(8);
        struct siginfo_ info = {
            .code = SI_KERNEL_,
            .fault.addr = cpu->pc,
        };
        runtime_report_guest_fault(SIGILL_, cpu->pc, ill_insn);
        deliver_signal(current, SIGILL_, info);
    } else if (interrupt == INT_BREAKPOINT) {
        {
            uint32_t brk_insn = 0;
            for (int j = 0; j < 4; j++) {
                uint8_t b;
                if (user_get(cpu->pc + j, b)) break;
                brk_insn |= (uint32_t)b << (j * 8);
            }
            runtime_report_guest_fault(SIGTRAP_, cpu->pc, brk_insn);
            if (g_trace_faults) { dump_stack(12); dump_maps(); }
        }
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = TRAP_BRKPT_,
            .fault.addr = cpu->pc,
        });
        unlock(&pids_lock);
    } else if (interrupt == INT_DEBUG) {
        lock(&pids_lock);
        send_signal(current, SIGTRAP_, (struct siginfo_) {
            .sig = SIGTRAP_,
            .code = TRAP_TRACE_,
        });
        unlock(&pids_lock);
    } else if (interrupt == INT_TIMER) {
        // timer handled below
    } else {
        fault_context("ENGINE_INTERRUPT tid=%d tgid=%d interrupt=%d last_syscall=%llu pc=%#llx",current->pid,current->tgid,interrupt,(unsigned long long)cpu->regs[8],(unsigned long long)cpu->pc);
        printk("EXIT[unhandled-int]: pid=%d interrupt=%d\n", current->pid, interrupt);
        sys_exit(interrupt);
    }

    receive_signals();
    struct tgroup *group = current->group;
    lock(&group->lock);
    while (group->stopped)
        wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
    unlock(&group->lock);
}

void dump_maps(void) {
    extern void proc_maps_dump(struct task *task, struct proc_data *buf);
    struct proc_data buf = {};
    proc_maps_dump(current, &buf);
    // Go a line at a time because /proc maps output can be very large.
    char *orig_data = buf.data;
    while (buf.size > 0) {
        size_t chunk_size = buf.size;
        if (chunk_size > 1024)
            chunk_size = 1024;
        printk("%.*s", chunk_size, buf.data);
        buf.data += chunk_size;
        buf.size -= chunk_size;
    }
    free(orig_data);
}

void dump_mem(addr_t start, uint_t len) {
    const int width = 8;
    for (addr_t addr = start; addr < start + len; addr += sizeof(dword_t)) {
        unsigned from_left = (addr - start) / sizeof(dword_t) % width;
        if (from_left == 0)
            printk("%012llx: ", (unsigned long long)addr);
        dword_t word;
        if (user_get(addr, word))
            break;
        printk("%08x ", word);
        if (from_left == width - 1)
            printk("\n");
    }
}

void dump_stack(int lines) {
    printk("stack at %llx, base at %llx, ip at %llx\n",
           (unsigned long long)current->cpu.sp,
           (unsigned long long)current->cpu.regs[29],  // x29 is frame pointer
           (unsigned long long)current->cpu.pc);
    dump_mem(current->cpu.sp, lines * sizeof(uint64_t) * 8);
}

// === Fast Path Implementations ===

// Fast path for fstat64 (syscall 80)
// Bypasses generic_statat path normalization when fd is already validated realfs
static inline int fast_fstat64(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t statbuf_addr = (addr_t)cpu->regs[1];

    // Quick validation: fd must be valid
    struct fd *fd = f_get_retain(fd_no);
    if (fd == NULL)
        return -1;  // Fall back to slow path

    // Fast path condition: fd is realfs (not adhoc, not procfs, etc.)
    if (fd->ops != &realfs_fdops || is_adhoc_fd(fd)) { fd_close(fd); return -1; }  // Fall back to slow path

    // Direct host fstat call (bypass generic layers)
    struct stat real_stat;
    if (fstat(fd->real_fd, &real_stat) < 0) {
        int error = errno_map(); fd_close(fd); return error;
    }
    fd_close(fd);

    // Convert to guest statbuf
    struct statbuf fake_stat = {};
    fake_stat.dev = dev_fake_from_real(real_stat.st_dev);
    fake_stat.inode = real_stat.st_ino;
    fake_stat.mode = real_stat.st_mode;
    fake_stat.nlink = real_stat.st_nlink;
    fake_stat.uid = real_stat.st_uid;
    fake_stat.gid = real_stat.st_gid;
    fake_stat.rdev = dev_fake_from_real(real_stat.st_rdev);
    fake_stat.size = real_stat.st_size;
    fake_stat.blksize = real_stat.st_blksize;
    fake_stat.blocks = real_stat.st_blocks;
    fake_stat.atime = platform_stat_atime_sec(&real_stat);
    fake_stat.mtime = platform_stat_mtime_sec(&real_stat);
    fake_stat.ctime = platform_stat_ctime_sec(&real_stat);
    fake_stat.atime_nsec = platform_stat_atime_nsec(&real_stat);
    fake_stat.mtime_nsec = platform_stat_mtime_nsec(&real_stat);
    fake_stat.ctime_nsec = platform_stat_ctime_nsec(&real_stat);

    // Convert to ARM64 stat structure
    struct stat_arm64 arm64stat = {};
    arm64stat.dev = fake_stat.dev;
    arm64stat.ino = fake_stat.inode;
    arm64stat.mode = fake_stat.mode;
    arm64stat.nlink = fake_stat.nlink;
    arm64stat.uid = fake_stat.uid;
    arm64stat.gid = fake_stat.gid;
    arm64stat.rdev = fake_stat.rdev;
    arm64stat.__pad1 = 0;
    arm64stat.size = fake_stat.size;
    arm64stat.blksize = fake_stat.blksize;
    arm64stat.__pad2 = 0;
    arm64stat.blocks = fake_stat.blocks;
    arm64stat.atime_ = fake_stat.atime;
    arm64stat.atime_nsec = fake_stat.atime_nsec;
    arm64stat.mtime_ = fake_stat.mtime;
    arm64stat.mtime_nsec = fake_stat.mtime_nsec;
    arm64stat.ctime_ = fake_stat.ctime;
    arm64stat.ctime_nsec = fake_stat.ctime_nsec;
    arm64stat.__unused4 = 0;
    arm64stat.__unused5 = 0;

    // Copy to user space
    if (user_put(statbuf_addr, arm64stat))
        return _EFAULT;

    return 0;  // Success
}

static inline bool syscall_should_surface_eintr(void) {
    if (current->group->doing_group_exit)
        return true;
    if (current->sighand != NULL) {
        lock(&current->sighand->lock);
        bool pending = !!(current->pending & ~current->blocked);
        unlock(&current->sighand->lock);
        if (pending)
            return true;
    }
    return false;
}

// Fast path for read (syscall 63) - small buffers only
static inline int fast_read(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t buf_addr = (addr_t)cpu->regs[1];
    dword_t size = (dword_t)cpu->regs[2];

    // Fast path condition: small buffer (≤ 4KB) and realfs fd
    if (size > 4096)
        return -1;

    struct fd *fd = f_get_retain(fd_no);
    if (fd == NULL) return -1;
    // Memfd offsets/writes must serialize with size-seal enforcement.
    if (fd->ops != &realfs_fdops || fd->memfd) { fd_close(fd); return -1; }

    // Direct host read. Retry spurious host EINTR, but surface guest signals
    // (especially exit_group SIGKILL) so helper threads blocked in pipes can
    // leave cleanly instead of being detached by the exit safety valve.
    char buf[4096];
    ssize_t res;
    do {
        if (current->group->doing_group_exit) { fd_close(fd); return _EINTR; }
        res = read(fd->real_fd, buf, size);
    } while (res < 0 && errno == EINTR && !syscall_should_surface_eintr());

    int error = res < 0 ? errno_map() : 0;
    fd_close(fd);
    if (error) return error;

    // Copy to guest memory
    if (res > 0 && user_write(buf_addr, buf, res))
        return _EFAULT;

    return res;
}

// Fast path for write (syscall 64) - small buffers only
static inline int fast_write(struct cpu_state *cpu) {
    fd_t fd_no = (fd_t)cpu->regs[0];
    addr_t buf_addr = (addr_t)cpu->regs[1];
    dword_t size = (dword_t)cpu->regs[2];

    // Fast path condition: small buffer (≤ 4KB) and realfs fd
    if (size > 4096)
        return -1;

    struct fd *fd = f_get_retain(fd_no);
    if (fd == NULL) return -1;
    // Memfd offsets/writes must serialize with size-seal enforcement.
    if (fd->ops != &realfs_fdops || fd->memfd) { fd_close(fd); return -1; }

    // Copy from guest memory
    char buf[4096];
    if (user_read(buf_addr, buf, size)) { fd_close(fd); return _EFAULT; }

    // Direct host write (with EINTR retry)
    ssize_t res;
    do {
        res = write(fd->real_fd, buf, size);
    } while (res < 0 && errno == EINTR);

    int error = res < 0 ? errno_map() : 0;
    fd_close(fd);
    if (error) return error;

    if (res > 0) runtime_capture_output(fd_no,buf,res);
    return res;
}
