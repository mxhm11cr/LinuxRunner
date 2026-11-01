#define _GNU_SOURCE
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){assert(argc==3);void *h=dlopen(argv[1],RTLD_NOW);if(!h){const char *error=dlerror();fprintf(stderr,"DLOPEN_REJECTED %s\n",error);if(!strcmp(argv[2],"reject")&&strstr(error,"executable stack")){puts("DLOPEN_POLICY_PASS modern glibc rejects late stack upgrade");return 0;}return 1;}assert(!strcmp(argv[2],"allow"));int (*f)(void)=dlsym(h,"execstack_library");assert(f&&f()==42);assert(!dlclose(h));puts("DLOPEN_PASS unmodified executable-stack DSO in an executable-stack process");return 0;}
