#include "app/LinuxRunner/FileTransfer.h"
#include "app/LinuxRunner/FileArchive.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <time.h>
#include <stdlib.h>
struct state { bool cancel,mutate; int source; };
static void interrupt_copy(void *context,uint64_t bytes,uint64_t total){(void)context;(void)total;if(bytes>=262144)_exit(42);}
static bool cancelled(void *p){return ((struct state *)p)->cancel;}
static void progress(void *p,uint64_t bytes,uint64_t total){(void)total;struct state *s=p;if(bytes>=262144){if(s->source>=0){if(s->mutate){char byte='!';assert(pwrite(s->source,&byte,1,0)==1);s->mutate=false;}}else s->cancel=true;}}
static void text(int d,const char *name,const char *value){int fd=openat(d,name,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(write(fd,value,strlen(value))==(ssize_t)strlen(value));close(fd);}
static void equal(int a,int b){char x[65536],y[65536];assert(lseek(a,0,SEEK_SET)==0&&lseek(b,0,SEEK_SET)==0);for(;;){ssize_t n=read(a,x,sizeof x),m=read(b,y,sizeof y);assert(n>=0&&n==m&&!memcmp(x,y,n));if(!n)break;}}
int main(int argc,char **argv){assert(argc==2);int root=open(argv[1],O_RDONLY|O_DIRECTORY);assert(root>=0);
 assert(mkdirat(root,"nested",0700)==0);assert(mkdirat(root,"nested/deeper",0700)==0);
 int directory=lr_open_subdirectory(root,"nested/deeper");assert(directory>=0);
 assert(lr_open_subdirectory(root,"../escape")==-1&&errno==EINVAL);
 assert(lr_open_subdirectory(root,"nested//deeper")==-1&&errno==EINVAL);
 assert(symlinkat("nested",root,"alias")==0);assert(lr_open_subdirectory(root,"alias/deeper")==-1);
 int source=openat(root,"source.bin",O_RDWR|O_CREAT|O_EXCL,0600);assert(source>=0);char buffer[65536];for(unsigned i=0;i<sizeof buffer;i++)buffer[i]=(i*13)%251;
 for(unsigned i=0;i<1024;i++)assert(write(source,buffer,sizeof buffer)==sizeof buffer);assert(!fsync(source));assert(lseek(source,0,SEEK_SET)==0);
 struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);assert(!lr_copy_in(source,directory,"résumé-مرحبا.bin"));clock_gettime(CLOCK_MONOTONIC,&b);
 int output=openat(root,"export.bin",O_RDWR|O_CREAT|O_EXCL,0600);assert(output>=0);assert(!lr_copy_out(directory,"résumé-مرحبا.bin",output));equal(source,output);
 close(directory);directory=lr_open_subdirectory(root,"nested/deeper");assert(directory>=0);int reopened=openat(directory,"résumé-مرحبا.bin",O_RDONLY);assert(reopened>=0);equal(source,reopened);close(reopened);
 // Cancellation never publishes a partial destination, and removes its stage.
 struct state stop={.source=-1};struct lr_transfer_control control={cancelled,progress,&stop};assert(lseek(source,0,SEEK_SET)==0);assert(lr_copy_in_controlled(source,directory,"cancelled.bin",&control)==ECANCELED);assert(faccessat(directory,"cancelled.bin",F_OK,0)==-1);
 struct state changing={.mutate=true,.source=source};control.context=&changing;assert(lseek(source,0,SEEK_SET)==0);assert(lr_copy_in_controlled(source,directory,"changed.bin",&control)==EBUSY);assert(faccessat(directory,"changed.bin",F_OK,0)==-1);
 DIR *scan=fdopendir(dup(directory));assert(scan);struct dirent *entry;while((entry=readdir(scan)))assert(strncmp(entry->d_name,".import-",8));closedir(scan);
 // Forced process exit leaves only a private stage, never a partial Shared file.
 assert(mkdirat(root,"private-staging",0700)==0);int stage=lr_open_subdirectory(root,"private-staging");assert(stage>=0);
 pid_t interrupted=fork();assert(interrupted>=0);if(!interrupted){int input=openat(root,"source.bin",O_RDONLY);struct lr_transfer_control crash={.progress=interrupt_copy};lr_copy_in_staged(input,directory,"interrupted.bin",stage,&crash);_exit(1);}
 int interrupted_status;assert(waitpid(interrupted,&interrupted_status,0)==interrupted&&WEXITSTATUS(interrupted_status)==42);assert(faccessat(directory,"interrupted.bin",F_OK,0)==-1);
 text(stage,"user-file","must remain");text(stage,".import-11111111-22222222","published contents");assert(!linkat(stage,".import-11111111-22222222",directory,"published-before-exit",0));assert(!lr_clean_import_staging(stage));assert(!faccessat(directory,"published-before-exit",F_OK,0));assert(!lr_clean_import_staging(stage));assert(faccessat(stage,"user-file",F_OK,0)==0);DIR *stages=fdopendir(dup(stage));assert(stages);while((entry=readdir(stages)))assert(strncmp(entry->d_name,".import-",8));closedir(stages);close(stage);
 // Concurrent import attempts preserve the first complete file.
 pid_t children[2];for(int i=0;i<2;i++){children[i]=fork();assert(children[i]>=0);if(!children[i]){int input=openat(root,"source.bin",O_RDONLY);int result=lr_copy_in(input,directory,"exclusive.bin");close(input);_exit(result);}}
 int codes[2];for(int i=0;i<2;i++){int status;assert(waitpid(children[i],&status,0)==children[i]&&WIFEXITED(status));codes[i]=WEXITSTATUS(status);}assert((codes[0]==0&&codes[1]==EEXIST)||(codes[1]==0&&codes[0]==EEXIST));
 text(root,"nested/note.txt","Linux-generated file\n");text(root,"nested/image.png","\211PNG\r\n\032\nreal fixture bytes");
 // Archive a smaller directory so binary-copy evidence need not enter Git.
 assert(mkdirat(root,"documents",0700)==0);text(root,"documents/hello.txt","round-trip\n");text(root,"documents/写真.txt","unicode\n");assert(mkdirat(root,"documents/empty",0700)==0);
 locale_t original=uselocale((locale_t)0);
 int zip=openat(root,"documents.zip",O_RDWR|O_CREAT|O_EXCL,0600);assert(zip>=0);assert(!lr_archive_directory(root,"documents",zip,NULL));assert(uselocale((locale_t)0)==original);close(zip);
 assert(symlinkat("../source.bin",root,"documents/escape")==0);zip=openat(root,"refused.zip",O_RDWR|O_CREAT|O_EXCL,0600);assert(zip>=0);assert(lr_archive_directory(root,"documents",zip,NULL)==EINVAL);close(zip);assert(unlinkat(root,"refused.zip",0)==0);
 struct rusage usage;getrusage(RUSAGE_SELF,&usage);printf("FILE_EXCHANGE_PASS bytes=67108864 copy_seconds=%.6f host_peak_rss=%ld unicode=PASS cancellation=PASS concurrent_change=PASS collision=PASS restart_reopen=PASS archive=PASS interrupted_private_stage_recovery=PASS published_hardlink_preserved=PASS traversal=REFUSED\n",(b.tv_sec-a.tv_sec)+(b.tv_nsec-a.tv_nsec)/1e9,usage.ru_maxrss);
 close(source);close(output);close(directory);close(root);return 0;}
