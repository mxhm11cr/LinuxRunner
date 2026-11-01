#include "DebianBootstrap.h"
#include <archive.h>
#include <archive_entry.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define MEMBER_LIMIT (128U*1024U*1024U)
#define TOTAL_LIMIT (1024ULL*1024ULL*1024ULL)
struct bootstrap {
    const struct lr_debian_bootstrap_options *options;
    long index;
    size_t count;
    char package[256], member[256], entry[512], destination[512];
    const char *stage;
    char *error;
    size_t error_size;
    int result, underlying_errno;
};
static void text(char *out,size_t size,const char *input) {
    snprintf(out,size,"%s",input?input:"");
    for(char *p=out;*p;p++)if((unsigned char)*p<32||*p=='"')*p='?';
}
static void log_event(struct bootstrap *s,int severity,const char *format,...) {
    if(!s->options||!s->options->log)return;
    char detail[1536],line[4096];va_list args;va_start(args,format);vsnprintf(detail,sizeof detail,format,args);va_end(args);
    snprintf(line,sizeof line,"package=%s index=%ld count=%zu stage=%s member=\"%s\" entry=\"%s\" destination=\"%s\" %s",s->package,s->index,s->count,s->stage?s->stage:"none",s->member,s->entry,s->destination,detail);
    s->options->log(s->options->context,severity,line);
}
static int fail(struct bootstrap *s,struct archive *a,const char *category,const char *operation,int code,const char *reason) {
    int underlying=a?archive_errno(a):0;
    const char *description=reason?reason:(a?archive_error_string(a):NULL);
    s->result=code; s->underlying_errno=underlying;
    char clean[512];text(clean,sizeof clean,description?description:"operation failed without library diagnostic");
    log_event(s,0,"result=FAIL category=%s operation=%s code=%d archive_errno=%d system_errno=%d cancelled=%d reason=\"%s\"",category,operation,code,underlying,errno,code==ECANCELED,clean);
    if(s->error&&s->error_size)snprintf(s->error,s->error_size,"Debian bootstrap: %s package=%s index=%ld stage=%s member=%s entry=%s operation=%s code=%d archive_errno=%d: %s",category,s->package,s->index,s->stage,s->member,s->entry,operation,code,underlying,clean);
    return -1;
}
static int cancelled(struct bootstrap *s) {
    if(s->options&&s->options->cancel&&s->options->cancel(s->options->context))return fail(s,NULL,"cancellation","cancel",ECANCELED,"Installation cancelled by user");
    return 0;
}
static int safe_path(const char *p) {
    if(!p||*p=='/'||strlen(p)>4095)return 0;
    for(const char *q=p;*q;){const char *end=strchr(q,'/');size_t n=end?(size_t)(end-q):strlen(q);if(n==2&&q[0]=='.'&&q[1]=='.')return 0;if(!end)break;q=end+1;}return 1;
}
static int file(struct bootstrap *s,struct archive *a,const char *path,const char *data,size_t size,int mode) {
    text(s->destination,sizeof s->destination,path);
    struct archive_entry *e=archive_entry_new();if(!e)return fail(s,NULL,"allocation","archive_entry_new",ENOMEM,NULL);
    archive_entry_set_pathname(e,path);archive_entry_set_filetype(e,AE_IFREG);archive_entry_set_perm(e,mode);archive_entry_set_size(e,size);archive_entry_set_uid(e,0);archive_entry_set_gid(e,0);archive_entry_set_mtime(e,0,0);
    errno=0;int r=archive_write_header(a,e);archive_entry_free(e);
    if(r!=ARCHIVE_OK)return fail(s,a,"filesystem","archive_write_header",archive_errno(a)>0?archive_errno(a):EIO,NULL);
    errno=0;if(archive_write_data(a,data,size)!=(la_ssize_t)size)return fail(s,a,"filesystem","archive_write_data",archive_errno(a)>0?archive_errno(a):EIO,NULL);
    return 0;
}
static void *member(struct bootstrap *s,struct archive *a,size_t *size) {
    size_t capacity=65536,length=0;char *buffer=malloc(capacity);
    if(!buffer){fail(s,NULL,"allocation","member_allocate",ENOMEM,NULL);return NULL;}
    for(;;){
        if(cancelled(s)){free(buffer);return NULL;}
        if(length==capacity){if(capacity==MEMBER_LIMIT){fail(s,a,"validation","member_size_limit",EFBIG,"Archive member exceeds 128 MiB limit");free(buffer);return NULL;}capacity*=2;char *grown=realloc(buffer,capacity);if(!grown){fail(s,NULL,"allocation","member_grow",ENOMEM,NULL);free(buffer);return NULL;}buffer=grown;}
        errno=0;la_ssize_t n=archive_read_data(a,buffer+length,capacity-length);
        if(n<0){fail(s,a,"decompression","archive_read_data",EBADMSG,NULL);free(buffer);return NULL;}
        if(!n)break;length+=(size_t)n;
    }
    *size=length;return buffer;
}
static struct archive *reader(struct bootstrap *s,int outer) {
    struct archive *a=archive_read_new();if(!a){fail(s,NULL,"allocation","archive_read_new",ENOMEM,NULL);return NULL;}
    // Only supported in-process codecs. Registering all filters can silently
    // install external executable fallbacks, which are invalid on stock iOS.
    archive_read_support_filter_none(a);
    int gzip=archive_read_support_filter_gzip(a),xz=archive_read_support_filter_xz(a);
    if(gzip!=ARCHIVE_OK||xz!=ARCHIVE_OK){fail(s,a,"codec_configuration","register_builtin_filters",ENOTSUP,"Built-in gzip/XZ decoder unavailable; external codec executables are forbidden");archive_read_free(a);return NULL;}
    int r=outer?archive_read_support_format_ar(a):archive_read_support_format_tar(a);
    if(r!=ARCHIVE_OK){fail(s,a,"archive_format","register_format",ENOTSUP,NULL);archive_read_free(a);return NULL;}
    return a;
}
static int next(struct bootstrap *s,struct archive *a,struct archive_entry **e,int trace) {
    if(cancelled(s))return ARCHIVE_FATAL;
    errno=0;int r=archive_read_next_header(a,e);
    if(r==ARCHIVE_OK){text(s->entry,sizeof s->entry,archive_entry_pathname(*e));if(trace)log_event(s,3,"format=\"%s\" filter=\"%s\" type=%o size=%lld operation=read_header result=PASS",archive_format_name(a),archive_filter_name(a,0),archive_entry_filetype(*e),(long long)archive_entry_size(*e));}
    else if(r!=ARCHIVE_EOF)fail(s,a,"archive_parse","archive_read_next_header",EBADMSG,NULL);
    return r;
}
static void field(const char *control,const char *key,char *out,size_t size) {
    const char *s=control;size_t n=strlen(key);*out=0;while(*s){if(!strncmp(s,key,n)&&s[n]==':'){s+=n+1;while(*s==' ')s++;size_t len=strcspn(s,"\r\n");if(len>=size)len=size-1;memcpy(out,s,len);out[len]=0;return;}s=strchr(s,'\n');if(!s)break;s++;}
}
int lr_debian_bootstrap_diagnosed(const char *const *packages,size_t count,const char *output,const struct lr_debian_bootstrap_options *options,char *error,size_t error_size) {
    struct bootstrap state={.options=options,.index=-1,.count=count,.stage="writer_setup",.error=error,.error_size=error_size};struct bootstrap *s=&state;
    if(error&&error_size)*error=0;
    struct archive *writer=archive_write_new();if(!writer){fail(s,NULL,"allocation","archive_write_new",ENOMEM,NULL);return s->result;}
    char *status=NULL;size_t status_size=0;uint64_t total=0;
    text(s->destination,sizeof s->destination,output);
    log_event(s,2,"operation=start libarchive=\"%s\" host_page=%ld guest_page=4096 extraction=guest_tar_only",archive_version_string(),sysconf(_SC_PAGESIZE));
    if(cancelled(s))goto done;
    if(archive_write_set_format_pax_restricted(writer)!=ARCHIVE_OK||archive_write_set_format_option(writer,"pax","hdrcharset","BINARY")!=ARCHIVE_OK||archive_write_add_filter_gzip(writer)!=ARCHIVE_OK){fail(s,writer,"codec_configuration","writer_configure",ENOTSUP,NULL);goto done;}
    // Linux filenames are byte strings. Preserve them in PAX rather than
    // converting through the native app process locale (normally C on iOS).
    log_event(s,2,"operation=filename_policy hdrcharset=BINARY locale_conversion=disabled");
    errno=0;if(archive_write_open_filename(writer,output)!=ARCHIVE_OK){fail(s,writer,"filesystem","writer_open",archive_errno(writer)>0?archive_errno(writer):EIO,NULL);goto done;}
    struct archive_entry *root=archive_entry_new();if(!root){fail(s,NULL,"allocation","root_entry",ENOMEM,NULL);goto done;}
    archive_entry_set_pathname(root,".");archive_entry_set_filetype(root,AE_IFDIR);archive_entry_set_perm(root,0755);archive_entry_set_mtime(root,0,0);
    errno=0;int root_result=archive_write_header(writer,root);archive_entry_free(root);if(root_result!=ARCHIVE_OK){fail(s,writer,"filesystem","root_write",EIO,NULL);goto done;}
    for(size_t i=0;i<count;i++) {
        s->index=(long)i;s->stage="package_open";s->member[0]=s->entry[0]=s->destination[0]=0;
        const char *base=strrchr(packages[i],'/');text(s->package,sizeof s->package,base?base+1:packages[i]);
        if(options&&options->progress&&options->progress(options->context,i,count)){fail(s,NULL,"cancellation","progress_cancel",ECANCELED,"Installation cancelled by user");goto done;}
        struct stat st;errno=0;if(lstat(packages[i],&st)||!S_ISREG(st.st_mode)){fail(s,NULL,"filesystem","package_stat",errno?errno:EINVAL,"Package path must be a readable regular file, not a symlink");goto done;}
        log_event(s,2,"operation=package_begin path=\"%s\" bytes=%lld mode=%o",packages[i],(long long)st.st_size,st.st_mode&0777);
        struct archive *deb=reader(s,1);if(!deb)goto done;
        errno=0;if(archive_read_open_filename(deb,packages[i],65536)!=ARCHIVE_OK){fail(s,deb,"archive_open","package_open",EBADMSG,NULL);archive_read_free(deb);goto done;}
        void *payload=NULL,*controls=NULL;size_t plen=0,clen=0;struct archive_entry *entry;int step;char payload_name[256]={0},control_name[256]={0};
        s->stage="deb_members";
        while((step=next(s,deb,&entry,0))==ARCHIVE_OK){
            const char *name=archive_entry_pathname(entry);text(s->member,sizeof s->member,name);
            log_event(s,2,"format=\"%s\" filter=\"%s\" type=%o size=%lld operation=ar_member",archive_format_name(deb),archive_filter_name(deb,0),archive_entry_filetype(entry),(long long)archive_entry_size(entry));
            if(!strncmp(name,"data.tar",8)&&!payload){payload=member(s,deb,&plen);text(payload_name,sizeof payload_name,name);if(!payload)break;}
            else if(!strncmp(name,"control.tar",11)&&!controls){controls=member(s,deb,&clen);text(control_name,sizeof control_name,name);if(!controls)break;}
            else if(archive_read_data_skip(deb)!=ARCHIVE_OK){fail(s,deb,"archive_parse","skip_member",EBADMSG,NULL);break;}
        }
        archive_read_free(deb);
        if(s->result||step!=ARCHIVE_EOF||!payload||!controls){if(!s->result)fail(s,NULL,"archive_structure","deb_required_members",EBADMSG,"Missing control.tar or data.tar member");free(payload);free(controls);goto done;}
        struct archive *control=NULL,*data=NULL;char *text_control=NULL,*list=NULL;size_t list_size=0;char name[256]={0},arch[64]={0},multi[64]={0},info[340]={0};
        s->stage="control_open";text(s->member,sizeof s->member,control_name);s->entry[0]=0;
        control=reader(s,0);if(!control)goto package_error;
        errno=0;if(archive_read_open_memory(control,controls,clen)!=ARCHIVE_OK){fail(s,control,"decompression","control_open_memory",EBADMSG,NULL);goto package_error;}
        s->stage="control_metadata";
        while((step=next(s,control,&entry,0))==ARCHIVE_OK){const char *path=archive_entry_pathname(entry);while(!strncmp(path,"./",2))path+=2;
            if(!strcmp(path,"control")){size_t len;char *content=member(s,control,&len);if(!content)goto package_error;text_control=malloc(len+1);if(!text_control){free(content);fail(s,NULL,"allocation","control_text",ENOMEM,NULL);goto package_error;}memcpy(text_control,content,len);text_control[len]=0;free(content);break;}}
        if(!text_control){if(!s->result)fail(s,NULL,"archive_structure","control_metadata",EBADMSG,"control file missing");goto package_error;}
        field(text_control,"Package",name,sizeof name);field(text_control,"Architecture",arch,sizeof arch);field(text_control,"Multi-Arch",multi,sizeof multi);
        if(!*name||strspn(name,"abcdefghijklmnopqrstuvwxyz0123456789+.-")!=strlen(name)||(strcmp(arch,"arm64")&&strcmp(arch,"all"))){fail(s,NULL,"validation","package_metadata",EINVAL,"Invalid package name or non-ARM64/all architecture");goto package_error;}
        text(s->package,sizeof s->package,name);log_event(s,2,"operation=control_parse architecture=%s format=\"%s\" filter=\"%s\" result=PASS",arch,archive_format_name(control),archive_filter_name(control,0));
        archive_read_free(control);control=NULL;
        snprintf(info,sizeof info,"%s%s%s",name,!strcmp(multi,"same")?":":"",!strcmp(multi,"same")?arch:"");
        size_t add=strlen(text_control)+strlen("Status: install ok unpacked\n\n")+1;char *grown=realloc(status,status_size+add);if(!grown){fail(s,NULL,"allocation","dpkg_status",ENOMEM,NULL);goto package_error;}status=grown;status_size+=snprintf(status+status_size,add,"Status: install ok unpacked\n%s\n",text_control);
        s->stage="control_extract";control=reader(s,0);if(!control)goto package_error;
        if(archive_read_open_memory(control,controls,clen)!=ARCHIVE_OK){fail(s,control,"decompression","control_reopen",EBADMSG,NULL);goto package_error;}
        while((step=next(s,control,&entry,options&&options->trace_entries))==ARCHIVE_OK){const char *path=archive_entry_pathname(entry);while(!strncmp(path,"./",2))path+=2;if(!*path||!strcmp(path,".")||!strcmp(path,"control"))continue;
            if(!safe_path(path)||strchr(path,'/')||archive_entry_filetype(entry)!=AE_IFREG){fail(s,NULL,"validation","control_entry",EINVAL,"Unsafe control-file path or non-regular entry");goto package_error;}
            size_t len;char *contents=member(s,control,&len);if(!contents)goto package_error;char dest[512];snprintf(dest,sizeof dest,"var/lib/dpkg/info/%s.%s",info,path);int written=file(s,writer,dest,contents,len,archive_entry_perm(entry));free(contents);if(written)goto package_error;}
        if(step!=ARCHIVE_EOF)goto package_error;archive_read_free(control);control=NULL;
        s->stage="payload_open";text(s->member,sizeof s->member,payload_name);s->entry[0]=s->destination[0]=0;
        data=reader(s,0);if(!data)goto package_error;
        errno=0;if(archive_read_open_memory(data,payload,plen)!=ARCHIVE_OK){fail(s,data,"decompression","payload_open_memory",EBADMSG,NULL);goto package_error;}
        s->stage="payload_extract";size_t entries=0;
        while((step=next(s,data,&entry,options&&options->trace_entries))==ARCHIVE_OK){
            const char *path=archive_entry_pathname(entry);while(!strncmp(path,"./",2))path+=2;text(s->destination,sizeof s->destination,path);
            if(!safe_path(path)||(archive_entry_hardlink(entry)&&!safe_path(archive_entry_hardlink(entry)))){fail(s,NULL,"validation","payload_path",EINVAL,"Unsafe guest path/hardlink; host extraction refused");goto package_error;}
            la_int64_t entry_size=archive_entry_size(entry);if(entry_size<0||(uint64_t)entry_size>TOTAL_LIMIT-total){fail(s,NULL,"validation","total_size",EFBIG,"Guest payload exceeds 1 GiB limit");goto package_error;}total+=(uint64_t)entry_size;
            char normalized[4096];snprintf(normalized,sizeof normalized,"%s",path);size_t length=strlen(normalized);while(length&&normalized[length-1]=='/')normalized[--length]=0;
            size_t n=strlen(normalized)+4;char *l=realloc(list,list_size+n);if(!l){fail(s,NULL,"allocation","dpkg_file_list",ENOMEM,NULL);goto package_error;}list=l;list_size+=snprintf(list+list_size,n,"/%s\n",!*normalized||!strcmp(normalized,".")?".":normalized);
            archive_entry_set_mtime(entry,0,0);errno=0;if(archive_write_header(writer,entry)!=ARCHIVE_OK){fail(s,writer,"filesystem","payload_write_header",archive_errno(writer)>0?archive_errno(writer):EIO,NULL);goto package_error;}
            char buffer[65536];la_ssize_t bytes;
            while((bytes=archive_read_data(data,buffer,sizeof buffer))>0){if(cancelled(s))goto package_error;errno=0;if(archive_write_data(writer,buffer,bytes)!=bytes){fail(s,writer,"filesystem","payload_write_data",archive_errno(writer)>0?archive_errno(writer):EIO,NULL);goto package_error;}}
            if(bytes<0){fail(s,data,"decompression","payload_read_data",EBADMSG,NULL);goto package_error;}entries++;
        }
        if(step!=ARCHIVE_EOF)goto package_error;
        log_event(s,2,"operation=payload_complete format=\"%s\" filter=\"%s\" entries=%zu total_guest_bytes=%llu result=PASS",archive_format_name(data),archive_filter_name(data,0),entries,(unsigned long long)total);
        archive_read_free(data);data=NULL;
        char dest[512];snprintf(dest,sizeof dest,"var/lib/dpkg/info/%s.list",info);if(file(s,writer,dest,list?list:"",list_size,0644))goto package_error;
        free(list);free(text_control);free(payload);free(controls);continue;
package_error:
        if(control)archive_read_free(control);if(data)archive_read_free(data);free(list);free(text_control);free(payload);free(controls);goto done;
    }
    s->stage="environment_configuration";s->entry[0]=s->member[0]=0;
#define CONFIG(path,text,mode) do{const char *value=text;if(file(s,writer,path,value,strlen(value),mode))goto done;}while(0)
    if(file(s,writer,"var/lib/dpkg/status",status?status:"",status_size,0644))goto done;
    CONFIG("var/lib/dpkg/available","",0644);CONFIG("etc/hostname","linuxrunner\n",0644);
    CONFIG("etc/hosts","127.0.0.1 localhost linuxrunner\n::1 localhost ip6-localhost\n",0644);
    CONFIG("etc/resolv.conf","nameserver 1.1.1.1\nnameserver 8.8.8.8\n",0644);
    CONFIG("etc/passwd","root:x:0:0:root:/root:/bin/bash\n_apt:x:100:65534::/nonexistent:/usr/sbin/nologin\n",0644);
    CONFIG("etc/group","root:x:0:\nusers:x:100:\nnogroup:x:65534:\n",0644);CONFIG("etc/shadow","root:*:20000:0:99999:7:::\n",0600);
    CONFIG("etc/apt/sources.list","deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main\ndeb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://security.debian.org/debian-security trixie-security main\n",0644);
    CONFIG("usr/sbin/policy-rc.d","#!/bin/sh\n# No service manager/kernel is booted in this userspace environment.\nexit 101\n",0755);
    CONFIG("usr/lib/linuxrunner/initialize-debian.sh","#!/bin/sh\nset -eu\nexport DEBIAN_FRONTEND=noninteractive\nmkdir -p /tmp /run /proc /dev /root /usr/local /var/lib/linuxrunner /var/lib/dpkg/updates /var/lib/apt/lists/partial /var/cache/apt/archives/partial\nchmod 1777 /tmp\nif [ ! -f /var/lib/linuxrunner/initialized ]; then\n if [ ! -f /var/lib/linuxrunner/accounts-initialized ]; then\n cp /usr/share/base-passwd/passwd.master /etc/passwd\n cp /usr/share/base-passwd/group.master /etc/group\n if ! grep -q '^_apt:' /etc/passwd; then\n printf '_apt:x:100:65534::/nonexistent:/usr/sbin/nologin\\n' >>/etc/passwd\n fi\n touch /var/lib/linuxrunner/accounts-initialized\n fi\n echo 'DEBIAN_INIT: configuring authenticated official packages'\n dpkg --configure -a\n update-ca-certificates\n touch /var/lib/linuxrunner/initialized\n echo 'DEBIAN_INIT: PASS'\nfi\n",0755);
    CONFIG("usr/lib/linuxrunner/init.sh","#!/bin/sh\n/usr/lib/linuxrunner/initialize-debian.sh >/var/log/linuxrunner-initialize.log 2>&1\nwhile :; do sleep 3600; done\n",0755);
done:
    if(!s->result&&cancelled(s)) { /* cancellation recorded above */ }
    errno=0;int close_result=archive_write_close(writer);
    if(close_result!=ARCHIVE_OK&&!s->result){s->stage="writer_close";fail(s,writer,"filesystem","archive_write_close",archive_errno(writer)>0?archive_errno(writer):EIO,NULL);}
    if(!s->result)log_event(s,2,"operation=finish result=PASS total_guest_bytes=%llu cancelled=0",(unsigned long long)total);
    archive_write_free(writer);free(status);if(s->result)unlink(output);return s->result;
}
int lr_debian_bootstrap(const char *const *packages,size_t count,const char *output,int (*progress)(void *,size_t,size_t),void *context,char *error,size_t error_size) {
    struct lr_debian_bootstrap_options options={.progress=progress,.context=context};
    return lr_debian_bootstrap_diagnosed(packages,count,output,&options,error,error_size);
}

// ---- Arch Linux ARM ----------------------------------------------------------

static const char *const arch_metadata[] = {".PKGINFO", ".MTREE", ".BUILDINFO", ".CHANGELOG", ".INSTALL", NULL};

static int arch_is_metadata(const char *path) {
    for (int i = 0; arch_metadata[i]; i++) if (!strcmp(path, arch_metadata[i])) return 1;
    return 0;
}

static int arch_dir(struct bootstrap *s, struct archive *a, const char *path) {
    struct archive_entry *e = archive_entry_new(); if (!e) return fail(s, NULL, "allocation", "archive_entry_new", ENOMEM, NULL);
    archive_entry_set_pathname(e, path); archive_entry_set_filetype(e, AE_IFDIR); archive_entry_set_perm(e, 0755);
    archive_entry_set_uid(e, 0); archive_entry_set_gid(e, 0); archive_entry_set_mtime(e, 0, 0);
    int r = archive_write_header(a, e); archive_entry_free(e);
    return r == ARCHIVE_OK ? 0 : fail(s, a, "filesystem", "archive_write_dir", EIO, NULL);
}

int lr_arch_read_database(const char *path, char **out, size_t *out_size, char *error, size_t error_size) {
    struct bootstrap state = {.index = -1, .stage = "database", .error = error, .error_size = error_size}; struct bootstrap *s = &state;
    if (error && error_size) *error = 0;
    *out = NULL; *out_size = 0;
    struct archive *a = reader(s, 0); if (!a) return s->result;
    if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) { fail(s, a, "archive_open", "database_open", EBADMSG, NULL); archive_read_free(a); return s->result; }
    char *buf = NULL; size_t size = 0, capacity = 0; struct archive_entry *e; int step;
    char current[256] = {0};
    while ((step = next(s, a, &e, 0)) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        const char *slash = strchr(name, '/');
        if (!slash || archive_entry_filetype(e) != AE_IFREG || (strcmp(slash + 1, "desc") && strcmp(slash + 1, "depends"))) continue;
        size_t len; char *content = member(s, a, &len); if (!content) break;
        size_t dirlen = (size_t)(slash - name); if (dirlen >= sizeof current) dirlen = sizeof current - 1;
        int new_entry = strncmp(current, name, dirlen) || current[dirlen] != 0;
        size_t need = size + len + dirlen + 4;
        if (need > capacity) { size_t c = capacity ? capacity : 1 << 20; while (c < need) c *= 2; char *g = realloc(buf, c); if (!g) { free(content); fail(s, NULL, "allocation", "database_buffer", ENOMEM, NULL); break; } buf = g; capacity = c; }
        if (new_entry) { memcpy(current, name, dirlen); current[dirlen] = 0; buf[size++] = 0x1e; memcpy(buf + size, name, dirlen); size += dirlen; buf[size++] = '\n'; }
        memcpy(buf + size, content, len); size += len; buf[size++] = '\n';
        free(content);
        if (size > TOTAL_LIMIT) { fail(s, NULL, "validation", "database_size", EFBIG, "Repository database too large"); break; }
    }
    archive_read_free(a);
    if (s->result || step != ARCHIVE_EOF) { if (!s->result) fail(s, NULL, "archive_parse", "database_read", EBADMSG, NULL); free(buf); return s->result; }
    *out = buf; *out_size = size; return 0;
}

int lr_arch_bootstrap_diagnosed(const char *const *packages, const char *const *local_names, const char *const *local_descs,
                                size_t count, const char *output, const struct lr_debian_bootstrap_options *options,
                                char *error, size_t error_size) {
    struct bootstrap state = {.options = options, .index = -1, .count = count, .stage = "writer_setup", .error = error, .error_size = error_size}; struct bootstrap *s = &state;
    if (error && error_size) *error = 0;
    struct archive *writer = archive_write_new(); if (!writer) { fail(s, NULL, "allocation", "archive_write_new", ENOMEM, NULL); return s->result; }
    uint64_t total = 0;
    log_event(s, 2, "operation=start distribution=arch libarchive=\"%s\" extraction=guest_tar_only", archive_version_string());
    if (archive_write_set_format_pax_restricted(writer) != ARCHIVE_OK || archive_write_set_format_option(writer, "pax", "hdrcharset", "BINARY") != ARCHIVE_OK ||
        archive_write_add_filter_gzip(writer) != ARCHIVE_OK) { fail(s, writer, "codec_configuration", "writer_configure", ENOTSUP, NULL); goto done; }
    if (archive_write_open_filename(writer, output) != ARCHIVE_OK) { fail(s, writer, "filesystem", "writer_open", archive_errno(writer) > 0 ? archive_errno(writer) : EIO, NULL); goto done; }
    if (arch_dir(s, writer, ".") || arch_dir(s, writer, "var") || arch_dir(s, writer, "var/lib") || arch_dir(s, writer, "var/lib/pacman") ||
        arch_dir(s, writer, "var/lib/pacman/local") || arch_dir(s, writer, "usr") || arch_dir(s, writer, "usr/lib") || arch_dir(s, writer, "usr/lib/linuxrunner")) goto done;
    for (size_t i = 0; i < count; i++) {
        s->index = (long)i; s->stage = "package_open"; s->member[0] = s->entry[0] = s->destination[0] = 0;
        text(s->package, sizeof s->package, local_names[i]);
        if (strspn(local_names[i], "abcdefghijklmnopqrstuvwxyz0123456789+.-_:@") != strlen(local_names[i]) || !*local_names[i]) { fail(s, NULL, "validation", "package_name", EINVAL, "Invalid package name"); goto done; }
        if (options && options->progress && options->progress(options->context, i, count)) { fail(s, NULL, "cancellation", "progress_cancel", ECANCELED, "Installation cancelled by user"); goto done; }
        struct stat st; if (lstat(packages[i], &st) || !S_ISREG(st.st_mode)) { fail(s, NULL, "filesystem", "package_stat", errno ? errno : EINVAL, "Package path must be a readable regular file, not a symlink"); goto done; }
        struct archive *pkg = reader(s, 0); if (!pkg) goto done;
        if (archive_read_open_filename(pkg, packages[i], 65536) != ARCHIVE_OK) { fail(s, pkg, "archive_open", "package_open", EBADMSG, NULL); archive_read_free(pkg); goto done; }
        char *list = NULL, *install = NULL; size_t list_size = 0, install_size = 0; struct archive_entry *entry; int step; size_t entries = 0;
        s->stage = "payload_extract";
        while ((step = next(s, pkg, &entry, options && options->trace_entries)) == ARCHIVE_OK) {
            const char *path = archive_entry_pathname(entry); while (!strncmp(path, "./", 2)) path += 2;
            text(s->destination, sizeof s->destination, path);
            if (!strcmp(path, ".INSTALL")) { install = member(s, pkg, &install_size); if (!install) goto package_error; continue; }
            if (arch_is_metadata(path)) { if (archive_read_data_skip(pkg) != ARCHIVE_OK) { fail(s, pkg, "archive_parse", "skip_metadata", EBADMSG, NULL); goto package_error; } continue; }
            if (!safe_path(path) || (archive_entry_hardlink(entry) && !safe_path(archive_entry_hardlink(entry)))) { fail(s, NULL, "validation", "payload_path", EINVAL, "Unsafe guest path/hardlink; host extraction refused"); goto package_error; }
            la_int64_t entry_size = archive_entry_size(entry);
            if (entry_size < 0 || (uint64_t)entry_size > TOTAL_LIMIT * 2 - total) { fail(s, NULL, "validation", "total_size", EFBIG, "Guest payload exceeds 2 GiB limit"); goto package_error; }
            total += (uint64_t)entry_size;
            size_t n = strlen(path) + 2; char *l = realloc(list, list_size + n); if (!l) { fail(s, NULL, "allocation", "file_list", ENOMEM, NULL); goto package_error; }
            list = l; list_size += snprintf(list + list_size, n, "%s\n", path);
            archive_entry_set_mtime(entry, 0, 0);
            if (archive_write_header(writer, entry) != ARCHIVE_OK) { fail(s, writer, "filesystem", "payload_write_header", archive_errno(writer) > 0 ? archive_errno(writer) : EIO, NULL); goto package_error; }
            char buffer[65536]; la_ssize_t bytes;
            while ((bytes = archive_read_data(pkg, buffer, sizeof buffer)) > 0) {
                if (cancelled(s)) goto package_error;
                if (archive_write_data(writer, buffer, bytes) != bytes) { fail(s, writer, "filesystem", "payload_write_data", archive_errno(writer) > 0 ? archive_errno(writer) : EIO, NULL); goto package_error; }
            }
            if (bytes < 0) { fail(s, pkg, "decompression", "payload_read_data", EBADMSG, NULL); goto package_error; }
            entries++;
        }
        if (step != ARCHIVE_EOF) goto package_error;
        archive_read_free(pkg); pkg = NULL;
        log_event(s, 2, "operation=payload_complete entries=%zu install_script=%d total_guest_bytes=%llu result=PASS", entries, install != NULL, (unsigned long long)total);
        s->stage = "local_database";
        char dir[512], dest[600];
        snprintf(dir, sizeof dir, "var/lib/pacman/local/%s", local_names[i]);
        if (arch_dir(s, writer, dir)) goto package_error;
        snprintf(dest, sizeof dest, "%s/desc", dir); if (file(s, writer, dest, local_descs[i], strlen(local_descs[i]), 0644)) goto package_error;
        {
            size_t fl = list_size + 16; char *files = malloc(fl); if (!files) { fail(s, NULL, "allocation", "files_db", ENOMEM, NULL); goto package_error; }
            int fn = snprintf(files, fl, "%%FILES%%\n"); memcpy(files + fn, list ? list : "", list_size); fn += (int)list_size; files[fn++] = '\n';
            snprintf(dest, sizeof dest, "%s/files", dir); int w = file(s, writer, dest, files, (size_t)fn, 0644); free(files); if (w) goto package_error;
        }
        if (install) { snprintf(dest, sizeof dest, "%s/install", dir); if (file(s, writer, dest, install, install_size, 0644)) goto package_error; }
        free(list); free(install); continue;
package_error:
        if (pkg) archive_read_free(pkg); free(list); free(install); goto done;
    }
    s->stage = "environment_configuration"; s->entry[0] = s->member[0] = 0;
#define ARCH_CONFIG(path, value, mode) do { const char *v = (value); if (file(s, writer, path, v, strlen(v), mode)) goto done; } while (0)
    ARCH_CONFIG("var/lib/pacman/local/ALPM_DB_VERSION", "9\n", 0644);
    ARCH_CONFIG("etc/hostname", "linuxrunner\n", 0644);
    ARCH_CONFIG("etc/hosts", "127.0.0.1 localhost linuxrunner\n::1 localhost\n", 0644);
    ARCH_CONFIG("etc/resolv.conf", "nameserver 1.1.1.1\nnameserver 8.8.8.8\n", 0644);
    // First boot: what pacman's install scripts and ALPM hooks would have done
    // (no package manager ran while assembling), then the pacman keyring.
    ARCH_CONFIG("usr/lib/linuxrunner/initialize-arch.sh",
        "#!/bin/sh\n"
        "set -u\n"
        "mkdir -p /tmp /run /var/lib/linuxrunner /var/cache/pacman/pkg\n"
        "# Boot and setup may request initialization together. Kernel-released, rootfs-local lock.\n"
        "[ ! -L /var/lib/linuxrunner/arch-initialize.lock ] || exit 1\n"
        "exec 8>>/var/lib/linuxrunner/arch-initialize.lock || exit 1\n"
        "flock 8 || exit 1\n"
        "chmod 1777 /tmp\n"
        "[ -f /var/lib/linuxrunner/initialized ] && exit 0\n"
        "echo 'ARCH_INIT: system users and runtime files'\n"
        "systemd-sysusers || exit 1\n"
        "systemd-tmpfiles --create >/dev/null 2>&1\n"
        "systemd-machine-id-setup >/dev/null 2>&1\n"
        "ldconfig\n"
        "echo 'ARCH_INIT: package install scripts'\n"
        "for d in /var/lib/pacman/local/*/; do\n"
        " [ -f \"$d/install\" ] || continue\n"
        " v=$(sed -n '/^%VERSION%$/{n;p;q}' \"$d/desc\")\n"
        " ( . \"$d/install\"; command -v post_install >/dev/null 2>&1 && post_install \"$v\" ) >/dev/null 2>&1 || echo \"ARCH_INIT: post_install failed: $d\"\n"
        "done\n"
        "echo 'ARCH_INIT: pacman download sandbox'\n"
        "# pacman 7 confines its download process with Landlock and seccomp; LinuxRunner\n"
        "# implements neither yet, and pacman fails rather than run unconfined. Use the\n"
        "# documented switches for kernels without them; downloads still run as 'alpm'.\n"
        "sed -i -e 's/^#DisableSandboxFilesystem$/DisableSandboxFilesystem/' -e 's/^#DisableSandboxSyscalls$/DisableSandboxSyscalls/' /etc/pacman.conf\n"
        "echo 'ARCH_INIT: pacman mirrors'\n"
        "# The packaged mirrorlist names only mirror.archlinuxarm.org, a geographic\n"
        "# redirect. pacman moves to the next Server when a download fails or stalls, so\n"
        "# list official mirrors after it (packages stay signature-checked); only while\n"
        "# the list is still the packaged default.\n"
        "if [ \"$(grep -c '^Server' /etc/pacman.d/mirrorlist)\" = 1 ] && grep -q '^Server = http://mirror.archlinuxarm.org/' /etc/pacman.d/mirrorlist; then\n"
        " for m in dk.mirror de.mirror fl.us.mirror eu.mirror; do echo \"Server = http://$m.archlinuxarm.org/\\$arch/\\$repo\"; done >> /etc/pacman.d/mirrorlist\n"
        "fi\n"
        "echo 'ARCH_INIT: certificates and pacman keyring'\n"
        "update-ca-trust >/dev/null 2>&1\n"
        "pacman-key --init >/dev/null 2>&1 || exit 1\n"
        "pacman-key --populate archlinuxarm >/dev/null 2>&1 || exit 1\n"
        "touch /var/lib/linuxrunner/initialized\n"
        "echo 'ARCH_INIT: PASS'\n", 0755);
    ARCH_CONFIG("usr/lib/linuxrunner/init.sh",
        "#!/bin/sh\n/usr/lib/linuxrunner/initialize-arch.sh >/var/log/linuxrunner-initialize.log 2>&1\nwhile :; do sleep 3600; done\n", 0755);
done:
    if (archive_write_close(writer) != ARCHIVE_OK && !s->result) { s->stage = "writer_close"; fail(s, writer, "filesystem", "archive_write_close", archive_errno(writer) > 0 ? archive_errno(writer) : EIO, NULL); }
    if (!s->result) log_event(s, 2, "operation=finish distribution=arch result=PASS total_guest_bytes=%llu", (unsigned long long)total);
    archive_write_free(writer); if (s->result) unlink(output); return s->result;
}
