// ZIP export of an explicitly selected shared directory. No symlinks, sockets,
// devices or FIFO traversal; descriptors pin each directory while it is read.
#ifndef LR_FILE_ARCHIVE_H
#define LR_FILE_ARCHIVE_H
#include "FileTransfer.h"
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif
struct lr_archive_state { struct archive *writer; const struct lr_transfer_control *control; uint64_t bytes; unsigned entries; };
static int lr_archive_tree(struct lr_archive_state *state,int parent,const char *leaf,const char *path,unsigned depth) {
    if(depth>32||++state->entries>4096||strlen(path)>4096)return EFBIG;
    if(state->control&&state->control->cancelled&&state->control->cancelled(state->control->context))return ECANCELED;
    struct stat st;if(fstatat(parent,leaf,&st,AT_SYMLINK_NOFOLLOW)<0)return errno;
    if(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))return EINVAL;
    int fd=openat(parent,leaf,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|(S_ISDIR(st.st_mode)?O_DIRECTORY:0));if(fd<0)return errno;
    struct stat opened;if(fstat(fd,&opened)<0){int error=errno;close(fd);return error;}
    if(st.st_dev!=opened.st_dev||st.st_ino!=opened.st_ino){close(fd);return EBUSY;}
    if(S_ISREG(st.st_mode)&&(st.st_size<0||(uint64_t)st.st_size>512ULL*1024*1024-state->bytes)){close(fd);return EFBIG;}
    struct archive_entry *entry=archive_entry_new();if(!entry){close(fd);return ENOMEM;}
    // The bundled libarchive requires the update API to populate its native
    // pathname too. set_pathname_utf8 alone leaves that pointer NULL in ZIP.
    if(!archive_entry_update_pathname_utf8(entry,path)){archive_entry_free(entry);close(fd);return EILSEQ;}
    archive_entry_set_filetype(entry,S_ISDIR(st.st_mode)?AE_IFDIR:AE_IFREG);
    archive_entry_set_perm(entry,S_ISDIR(st.st_mode)?0755:0644);archive_entry_set_size(entry,S_ISREG(st.st_mode)?st.st_size:0);archive_entry_set_mtime(entry,st.st_mtime,0);
    int error=archive_write_header(state->writer,entry)==ARCHIVE_OK?0:EIO;archive_entry_free(entry);
    if(!error&&S_ISDIR(st.st_mode)) {
        int copy=dup(fd);DIR *directory=copy<0?NULL:fdopendir(copy);if(!directory){error=errno;if(copy>=0)close(copy);}
        else {
            struct dirent *child;
            while(!error){errno=0;child=readdir(directory);if(!child){if(errno)error=errno;break;}
                if(!strcmp(child->d_name,".")||!strcmp(child->d_name,".."))continue;
                if(!lr_valid_leaf(child->d_name)){error=EINVAL;break;}
                char next[4097];int n=snprintf(next,sizeof next,"%s/%s",path,child->d_name);if(n<0||n>=(int)sizeof next){error=ENAMETOOLONG;break;}
                error=lr_archive_tree(state,fd,child->d_name,next,depth+1);
            }
            closedir(directory);
        }
    } else if(!error) {
        char buffer[65536];uint64_t copied=0;
        while(!error) {
            if(state->control&&state->control->cancelled&&state->control->cancelled(state->control->context)){error=ECANCELED;break;}
            ssize_t count=read(fd,buffer,sizeof buffer);if(count<0){if(errno==EINTR)continue;error=errno;break;}if(!count)break;
            if((uint64_t)count>512ULL*1024*1024-state->bytes){error=EFBIG;break;}
            if(archive_write_data(state->writer,buffer,count)!=count){error=EIO;break;}
            state->bytes+=count;copied+=count;
            if(state->control&&state->control->progress)state->control->progress(state->control->context,state->bytes,0);
        }
        if(!error&&copied!=(uint64_t)st.st_size)error=EBUSY;
    }
    struct stat after;if(!error&&fstat(fd,&after)<0)error=errno;
    if(!error&&(after.st_size!=opened.st_size||after.st_mtime!=opened.st_mtime||after.st_ctime!=opened.st_ctime))error=EBUSY;
#ifdef __APPLE__
    if(!error&&(after.st_mtimespec.tv_nsec!=opened.st_mtimespec.tv_nsec||after.st_ctimespec.tv_nsec!=opened.st_ctimespec.tv_nsec))error=EBUSY;
#else
    if(!error&&(after.st_mtim.tv_nsec!=opened.st_mtim.tv_nsec||after.st_ctim.tv_nsec!=opened.st_ctim.tv_nsec))error=EBUSY;
#endif
    close(fd);return error;
}
static int lr_archive_directory_utf8(int directory,const char *leaf,int output,const struct lr_transfer_control *control) {
    if(!lr_valid_leaf(leaf))return EINVAL;
    struct stat st;if(fstatat(directory,leaf,&st,AT_SYMLINK_NOFOLLOW)<0)return errno;if(!S_ISDIR(st.st_mode))return ENOTDIR;
    struct archive *writer=archive_write_new();if(!writer)return ENOMEM;
    archive_write_set_format_zip(writer);
    int error=archive_write_set_options(writer,"zip:compression=store,zip:hdrcharset=UTF-8")==ARCHIVE_OK?0:EIO;
    if(!error&&archive_write_open_fd(writer,output)!=ARCHIVE_OK)error=EIO;
    struct lr_archive_state state={.writer=writer,.control=control};
    if(!error)error=lr_archive_tree(&state,directory,leaf,leaf,0);
    if(archive_write_close(writer)!=ARCHIVE_OK&&!error)error=EIO;
    archive_write_free(writer);if(!error&&fsync(output)<0)error=errno;return error;
}
static int lr_archive_directory(int directory,const char *leaf,int output,const struct lr_transfer_control *control) {
    // Set a locale on this worker only; never change the interpreter's process
    // locale or other concurrent transfers. Restore even on cancellation.
    locale_t utf8=newlocale(LC_CTYPE_MASK,"en_US.UTF-8",(locale_t)0);
    if(!utf8)return errno?:EILSEQ;
    locale_t previous=uselocale(utf8);
    if(!previous){int error=errno;freelocale(utf8);return error?:EILSEQ;}
    int error=lr_archive_directory_utf8(directory,leaf,output,control);
    uselocale(previous);freelocale(utf8);return error;
}
#endif
