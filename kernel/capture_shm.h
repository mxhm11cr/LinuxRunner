#ifndef KERNEL_CAPTURE_SHM_H
#define KERNEL_CAPTURE_SHM_H
// LinuxRunner extension, not a Linux prctl option. Called by the capture
// launcher before exec, while single-threaded. Future IPC_PRIVATE allocations
// are marked IPC_RMID when its last thread exits. Forked processes do not
// inherit the scope; exec and CLONE_THREAD preserve it. No existing or keyed
// segments are adopted. Unsupported runtimes must use socket capture.
#define LR_PR_CAPTURE_SHM 0x4c525348
#define LR_CAPTURE_SHM_VERSION 1
#endif
