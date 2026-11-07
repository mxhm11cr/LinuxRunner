#import <Foundation/Foundation.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "graphics/host_gpu_job.h"

static atomic_int live, released, peak;
@interface GPUJobProbe : NSObject
@end
@implementation GPUJobProbe
- (instancetype)init {
    self=[super init];
    if(self){int count=atomic_fetch_add(&live,1)+1;if(count>atomic_load(&peak))atomic_store(&peak,count);}
    return self;
}
- (void)dealloc { atomic_fetch_sub(&live,1);atomic_fetch_add(&released,1);[super dealloc]; }
@end
static void make_temporaries(void *unused) {
    (void)unused;
    for(int i=0;i<32;i++)(void)[[[GPUJobProbe alloc] init] autorelease];
}
static void *worker(void *unused) {
    (void)unused;
    // The parent pool stays alive across all jobs, as on a long-running worker.
    @autoreleasepool {
        for(int i=0;i<10000;i++) {
            lr_host_gpu_job(make_temporaries,NULL);
            assert(atomic_load(&live)==0);
        }
    }
    return NULL;
}
int main(void) {
    // This control shows that unscoped work retains its temporary objects.
    @autoreleasepool { make_temporaries(NULL);assert(atomic_load(&live)==32); }
    assert(atomic_load(&live)==0);
    atomic_store(&released,0);atomic_store(&peak,0);
    pthread_t thread;assert(pthread_create(&thread,NULL,worker,NULL)==0);
    assert(pthread_join(thread,NULL)==0);
    assert(atomic_load(&released)==320000);assert(atomic_load(&peak)==32);
    puts("PASS: 10,000 GPU jobs release 320,000 temporary objects before the next job");
}
