#!/usr/bin/env python3
"""Run inside an ARM64 Linux guest to check the restricted openat2 walk."""
import ctypes
import errno
import os
import tempfile
import threading

libc = ctypes.CDLL(None, use_errno=True)
SYS_OPENAT2 = 437
AT_FDCWD = -100
O_PATH = 0o10000000
O_DIRECTORY = 0o40000
O_NOFOLLOW = 0o100000
O_CLOEXEC = 0o2000000
RESOLVE_BENEATH = 0x08

class OpenHow(ctypes.Structure):
    _fields_ = [('flags', ctypes.c_uint64), ('mode', ctypes.c_uint64),
                ('resolve', ctypes.c_uint64)]

how = OpenHow(O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0, RESOLVE_BENEATH)

def call(dirfd, path):
    ctypes.set_errno(0)
    result = libc.syscall(SYS_OPENAT2, dirfd, os.fsencode(path), ctypes.byref(how), ctypes.sizeof(how))
    return result, ctypes.get_errno()

with tempfile.TemporaryDirectory(prefix='lr-openat2-') as root:
    os.makedirs(root + '/base/a/b')
    os.makedirs(root + '/outside')
    os.symlink(root + '/outside', root + '/base/escape')
    os.symlink('../outside', root + '/base/relative')
    os.symlink('b', root + '/base/a/inside-link')
    base = os.open(root + '/base', os.O_RDONLY | os.O_DIRECTORY)
    try:
        for path in ('a/', 'a/b/', './a//b/'):
            fd, error = call(base, path)
            assert fd >= 0, (path, error)
            assert os.path.samestat(os.fstat(fd), os.stat(root + '/base/a' + ('/b' if 'b' in path else '')))
            os.close(fd)
        for path in ('../outside', '/tmp', 'escape', 'relative', 'a/inside-link'):
            fd, error = call(base, path)
            assert fd == -1 and error in (errno.EXDEV, errno.ELOOP, errno.ENOTDIR), (path, fd, error)
        fd, error = call(-1, 'a')
        assert fd == -1 and error == errno.EBADF, (fd, error)
        # Race directory replacement against the confined walk. Never return a
        # descriptor for the external target even when names are exchanged.
        os.mkdir(root + '/outside/b')
        safe_inode = os.stat(root + '/base/a/b').st_ino
        outside_inode = os.stat(root + '/outside/b').st_ino
        failures = []
        def swap_directory():
            for _ in range(150):
                try:
                    os.rename(root + '/base/a', root + '/base/held')
                    os.symlink(root + '/outside', root + '/base/a')
                    os.unlink(root + '/base/a')
                    os.rename(root + '/base/held', root + '/base/a')
                except OSError as error:
                    failures.append(str(error))
                    return
        thread = threading.Thread(target=swap_directory)
        thread.start()
        for _ in range(300):
            fd, error = call(base, 'a/b/')
            if fd >= 0:
                inode = os.fstat(fd).st_ino
                assert inode == safe_inode and inode != outside_inode, inode
                os.close(fd)
            else:
                assert error in (errno.ENOENT, errno.ELOOP, errno.ENOTDIR, errno.EAGAIN), error
        thread.join()
        assert not failures, failures
        old = os.getcwd()
        try:
            os.chdir(root + '/base')
            fd, error = call(AT_FDCWD, 'a/b/')
            assert fd >= 0, (fd, error)
            os.close(fd)
        finally:
            os.chdir(old)
    finally:
        os.close(base)
print('OPENAT2_BENEATH_DIRECTORY_PASS nested nofollow escape fd cwd')
