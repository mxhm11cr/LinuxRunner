#ifndef LR_HOST_GPU_JOB_H
#define LR_HOST_GPU_JOB_H
// The GPU worker has no run loop to drain temporary Cocoa and Metal objects.
// Drain them after initialization and after each job, before its caller resumes.
static inline void lr_host_gpu_job(void (*run)(void *), void *argument) {
#ifdef __OBJC__
    @autoreleasepool { run(argument); }
#else
    run(argument);
#endif
}
#endif
