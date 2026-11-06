// Run the host interpreter with a native mailbox subscriber and input sender.
// Compile main.c with -Dmain=lr_cli_main, then link the host runtime libraries.
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "fs/kms_display.h"
extern int lr_cli_main(int,char *const[]);
extern void drm_set_display_size(unsigned,unsigned);
static atomic_bool finished;
static uint64_t token;
static pthread_t thread;
static void *watch(void *unused) {
    (void)unused;
    unsigned frames=0,w=0,h=0,tick=0;
    const char *output=getenv("LR_KMS_TEST_FRAME");
    const char *ready=getenv("LR_KMS_TEST_READY");
    while(!atomic_load(&finished)) {
        if(ready && access(ready,F_OK)) { usleep(10000); continue; }
        struct lr_kms_frame frame={0};
        if(lr_kms_display_take(token,&frame)) {
            frames++;w=frame.width;h=frame.height;
            if(output) { FILE *f=fopen(output,"wb");if(f){fwrite(frame.pixels,4,(size_t)w*h,f);fclose(f);} }
            free(frame.pixels);
        }
        if(w && ++tick%100==0) {
            struct lr_kms_input pointer={.type=1,.x=w/2,.y=h/2,.width=w,.height=h,.buttons=1};
            lr_kms_display_input(token,pointer);pointer.buttons=0;lr_kms_display_input(token,pointer);
            lr_kms_display_input(token,(struct lr_kms_input){.type=2,.keysym='a',.down=1});
            lr_kms_display_input(token,(struct lr_kms_input){.type=2,.keysym='a'});
        }
        usleep(10000);
    }
    fprintf(stderr,"NATIVE_HOST_DISPLAY frames=%u width=%u height=%u\n",frames,w,h);
    return NULL;
}
static void cleanup(void) { atomic_store(&finished,true);pthread_join(thread,NULL);lr_kms_display_end(token); }
int main(int argc,char *const argv[]) {
    drm_set_display_size(640,480);token=lr_kms_display_begin();
    pthread_create(&thread,NULL,watch,NULL);atexit(cleanup);
    return lr_cli_main(argc,argv);
}
