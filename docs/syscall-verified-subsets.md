## 873 verified subsets (Mac fixtures)

* dup/dup3/fcntl: invalid arguments, shared flags, atomic replacement, per-table CLOEXEC,
  full-table EMFILE, concurrent duplication/close and descriptor inheritance.
* pipe2 and proc/dev fd: cleanup, native errno, same-direction reopening, independent flags,
  inode DAC and descriptor transfer through Unix SCM_RIGHTS. Full proc reopen/O_PATH omitted.
* ppoll/pselect/epoll: bounds, invalid entries, duplicate readiness counts, signals, monotonic
  finite waits and existing edge/oneshot/bounded-batch regression contracts.
* getrlimit/setrlimit/prlimit64: cur/max validation, explicit/current/other PID, private
  process limits, unauthorized target rejection. Linux-visible limits are not host quotas.
* close_range: true UNSHARE, CLOEXEC table isolation; Linux ELF exec detaches CLONE_FILES
  sharing before closing CLOEXEC descriptors and restores sharing on failure.
* AArch64 BRK: SIGTRAP/TRAP_BRKPT, fault PC and register-preserving guest handler delivery.
* chdir/fchdir: search-only directory access and descriptor type validation.

These are focused contracts, not full syscall conformance or physical-device certification.

## Ancillary IPC continuation (Mac fixture contracts)

`socket_ancillary_contract.c` and `run-socket-ancillary-macos.py` cover receiver
NOFILE truncation, partial control buffers, MSG_PEEK/CLOEXEC, shared offsets,
SCM description lifetime across sender close, socket close/reuse during a blocked
receive, zero-byte stream sends, raw 64-bit iovec count and non-socket errors.
These are bounded subsets, not complete socket conformance. Receive-fault cleanup
keeps subsequent bytes and rights associated and releases imported host handles;
Linux stream payload retention on EFAULT remains a documented native-import gap.
The tests record that gap explicitly instead of calling it a pass.

## Process inspection and imported files continuation

`dumpable_contract.c`: GET/SET_DUMPABLE, thread sharing, fork inheritance, exec
reset/failed-exec rollback, UID/GID resets, sensitive cross-process proc reads and
proc ownership. `proc_task_links_contract.c`: live thread-directory nlink across
thread creation, fork and join; this corrects the original Chromium assertion.
`run-bind-exec-macos.py`: newly imported execute bits, nonexecutables and readonly
enforcement. Existing guest metadata is not migrated or overwritten.
