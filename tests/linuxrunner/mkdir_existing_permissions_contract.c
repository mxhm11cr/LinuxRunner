#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void exists(const char *path){errno=0;assert(mkdir(path,0755)==-1&&errno==EEXIST);}
int main(void){char base[]="/tmp/lr-mkdir-existing-XXXXXX";assert(mkdtemp(base));assert(!chmod(base,0755));char parent[256],child[256],missing[256],writable[256],link[256],target[256];
 snprintf(parent,sizeof parent,"%s/readonly",base);snprintf(child,sizeof child,"%s/readonly/existing",base);snprintf(missing,sizeof missing,"%s/readonly/missing",base);snprintf(writable,sizeof writable,"%s/writable",base);snprintf(link,sizeof link,"%s/writable/dangling",base);snprintf(target,sizeof target,"%s/writable/target",base);
 assert(!mkdir(parent,0755)&&!mkdir(child,0755)&&!mkdir(writable,0777)&&!chmod(writable,0777)&&!chmod(parent,0555));assert(!symlink("target",link));
 pid_t process=fork();assert(process>=0);if(!process){if(geteuid()==0){assert(!setgid(1000)&&!setuid(1000));}assert(geteuid()!=0);exists("/");exists(parent);exists(child);exists(link);errno=0;assert(mkdir(missing,0755)==-1&&errno==EACCES);assert(access(target,F_OK)==-1&&errno==ENOENT);assert(!mkdir(target,0755));struct stat st;assert(!stat(target,&st)&&st.st_uid==geteuid());_exit(0);}
 int status;assert(waitpid(process,&status,0)==process&&WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(!chmod(parent,0755));assert(!unlink(link)&&!rmdir(target)&&!rmdir(child)&&!rmdir(parent)&&!rmdir(writable)&&!rmdir(base));puts("MKDIR_EXISTING_PASS root/readonly-existing/dangling-symlink=EEXIST missing=EACCES normal-user-create=PASS");return 0;}
