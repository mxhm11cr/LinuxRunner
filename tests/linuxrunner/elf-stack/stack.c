#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int expected,thread_expected;
static int executable(const void *p) {
 FILE *f=fopen("/proc/self/maps","r");assert(f);char line[1024],permissions[8];unsigned long a,b;int result=-1;
 while(fgets(line,sizeof line,f))if(sscanf(line,"%lx-%lx %7s",&a,&b,permissions)==3&&(uintptr_t)p>=a&&(uintptr_t)p<b){result=permissions[2]=='x';break;}
 fclose(f);assert(result>=0);return result;
}
static int run_stack_code(void){_Alignas(16) uint32_t code[]={0x52800540,0xd65f03c0};assert(executable(code)==expected);__builtin___clear_cache((char *)code,(char *)(code+2));return ((int (*)(void))(uintptr_t)code)();}
__attribute__((noinline)) static void grow(unsigned n){volatile unsigned char padding[8192];padding[0]=n;padding[sizeof padding-1]=n;if(n)grow(n-1);else {assert(executable((const void *)padding)==expected);if(expected)assert(run_stack_code()==42);}assert(padding[0]==n);}
static void *thread(void *unused){(void)unused;int local;assert(executable(&local)==thread_expected);if(thread_expected)assert(run_stack_code()==42);return NULL;}
#ifdef STARTUP_LIBRARY
extern int execstack_library(void);
#endif
int main(int argc,char **argv){assert(argc==2);expected=atoi(argv[1]);thread_expected=getenv("LR_THREAD_EXPECT_EXEC")?atoi(getenv("LR_THREAD_EXPECT_EXEC")):expected;int local;assert(executable(&local)==expected);grow(12);pthread_t t;assert(!pthread_create(&t,NULL,thread,NULL)&&!pthread_join(t,NULL));
#ifdef STARTUP_LIBRARY
 assert(execstack_library()==42);
#endif
 printf("ELF_STACK_PASS expected_exec=%d thread_exec=%d main/grown/thread metadata and interpreted stack code\n",expected,thread_expected);return 0;}
