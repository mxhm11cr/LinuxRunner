// sysfs: only what describes LinuxRunner's DRM devices (fs/drm.c), in the
// layout libudev and libdrm read: the platform device "linuxrunner-drm" with
// the software minors card0 and renderD128, and "linuxrunner-virtio-gpu" with
// card1 and renderD129 (virtio-gpu, present only with ISH_VIRGL=1). Mounted at /sys when the device is
// enabled (ISH_SOFTWARE_DRM=1); nothing here describes host hardware.
#include <string.h>
#include <sys/stat.h>
#include "fs/proc.h"

#define LINK(fn, target) static int fn(struct proc_entry *UNUSED(entry), char *buf) { strcpy(buf, target); return 0; }
LINK(class_card0, "../../devices/platform/linuxrunner-drm/drm/card0")
LINK(class_render, "../../devices/platform/linuxrunner-drm/drm/renderD128")
LINK(char_card0, "../../devices/platform/linuxrunner-drm/drm/card0")
LINK(char_render, "../../devices/platform/linuxrunner-drm/drm/renderD128")
LINK(bus_device, "../../../devices/platform/linuxrunner-drm")
LINK(class_card1, "../../devices/platform/linuxrunner-virtio-gpu/drm/card1")
LINK(class_render129, "../../devices/platform/linuxrunner-virtio-gpu/drm/renderD129")
LINK(char_card1, "../../devices/platform/linuxrunner-virtio-gpu/drm/card1")
LINK(char_render129, "../../devices/platform/linuxrunner-virtio-gpu/drm/renderD129")
LINK(bus_virtio, "../../../devices/platform/linuxrunner-virtio-gpu")
LINK(platform_subsystem, "../../../bus/platform")
LINK(minor_subsystem, "../../../../../class/drm")
LINK(minor_device, "../..")

static int platform_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "DRIVER=linuxrunner\nOF_NAME=linuxrunner-drm\nOF_FULLNAME=/linuxrunner-drm\n"
                     "OF_COMPATIBLE_0=linuxrunner,software-kms\nOF_COMPATIBLE_N=1\nMODALIAS=of:Nlinuxrunner-drmT(null)Clinuxrunner,software-kms\n");
    return 0;
}
static int card0_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "MAJOR=226\nMINOR=0\nDEVNAME=dri/card0\nDEVTYPE=drm_minor\n");
    return 0;
}
static int render_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n");
    return 0;
}
static int virtio_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "DRIVER=virtio_gpu\nOF_NAME=linuxrunner-virtio-gpu\nOF_FULLNAME=/linuxrunner-virtio-gpu\n"
                     "OF_COMPATIBLE_0=linuxrunner,virtio-gpu\nOF_COMPATIBLE_N=1\nMODALIAS=of:Nlinuxrunner-virtio-gpuT(null)Clinuxrunner,virtio-gpu\n");
    return 0;
}
static int card1_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "MAJOR=226\nMINOR=1\nDEVNAME=dri/card1\nDEVTYPE=drm_minor\n");
    return 0;
}
static int render129_uevent(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "MAJOR=226\nMINOR=129\nDEVNAME=dri/renderD129\nDEVTYPE=drm_minor\n");
    return 0;
}
static int card1_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) { proc_printf(buf, "226:1\n"); return 0; }
static int render129_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) { proc_printf(buf, "226:129\n"); return 0; }
static int card0_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) { proc_printf(buf, "226:0\n"); return 0; }
static int render_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) { proc_printf(buf, "226:128\n"); return 0; }

static struct proc_children card0_children = PROC_CHILDREN({
    {"dev", .show = card0_dev},
    {"device", S_IFLNK, .readlink = minor_device},
    {"subsystem", S_IFLNK, .readlink = minor_subsystem},
    {"uevent", .show = card0_uevent},
});
static struct proc_children render_children = PROC_CHILDREN({
    {"dev", .show = render_dev},
    {"device", S_IFLNK, .readlink = minor_device},
    {"subsystem", S_IFLNK, .readlink = minor_subsystem},
    {"uevent", .show = render_uevent},
});
static struct proc_children card1_children = PROC_CHILDREN({
    {"dev", .show = card1_dev},
    {"device", S_IFLNK, .readlink = minor_device},
    {"subsystem", S_IFLNK, .readlink = minor_subsystem},
    {"uevent", .show = card1_uevent},
});
static struct proc_children render129_children = PROC_CHILDREN({
    {"dev", .show = render129_dev},
    {"device", S_IFLNK, .readlink = minor_device},
    {"subsystem", S_IFLNK, .readlink = minor_subsystem},
    {"uevent", .show = render129_uevent},
});
static struct proc_children virtio_minors_children = PROC_CHILDREN({
    {"card1", S_IFDIR, .children = &card1_children},
    {"renderD129", S_IFDIR, .children = &render129_children},
});
static struct proc_children virtio_device_children = PROC_CHILDREN({
    {"drm", S_IFDIR, .children = &virtio_minors_children},
    {"subsystem", S_IFLNK, .readlink = platform_subsystem},
    {"uevent", .show = virtio_uevent},
});
static struct proc_children minors_children = PROC_CHILDREN({
    {"card0", S_IFDIR, .children = &card0_children},
    {"renderD128", S_IFDIR, .children = &render_children},
});
static struct proc_children platform_device_children = PROC_CHILDREN({
    {"drm", S_IFDIR, .children = &minors_children},
    {"subsystem", S_IFLNK, .readlink = platform_subsystem},
    {"uevent", .show = platform_uevent},
});
static struct proc_children devices_platform_children = PROC_CHILDREN({
    {"linuxrunner-drm", S_IFDIR, .children = &platform_device_children},
    {"linuxrunner-virtio-gpu", S_IFDIR, .children = &virtio_device_children},
});
static struct proc_children devices_children = PROC_CHILDREN({
    {"platform", S_IFDIR, .children = &devices_platform_children},
});
static struct proc_children class_drm_children = PROC_CHILDREN({
    {"card0", S_IFLNK, .readlink = class_card0},
    {"card1", S_IFLNK, .readlink = class_card1},
    {"renderD128", S_IFLNK, .readlink = class_render},
    {"renderD129", S_IFLNK, .readlink = class_render129},
});
static struct proc_children class_children = PROC_CHILDREN({
    {"drm", S_IFDIR, .children = &class_drm_children},
});
static struct proc_children bus_platform_devices_children = PROC_CHILDREN({
    {"linuxrunner-drm", S_IFLNK, .readlink = bus_device},
    {"linuxrunner-virtio-gpu", S_IFLNK, .readlink = bus_virtio},
});
static struct proc_children bus_platform_children = PROC_CHILDREN({
    {"devices", S_IFDIR, .children = &bus_platform_devices_children},
});
static struct proc_children bus_children = PROC_CHILDREN({
    {"platform", S_IFDIR, .children = &bus_platform_children},
});
static struct proc_children dev_char_children = PROC_CHILDREN({
    {"226:0", S_IFLNK, .readlink = char_card0},
    {"226:1", S_IFLNK, .readlink = char_card1},
    {"226:128", S_IFLNK, .readlink = char_render},
    {"226:129", S_IFLNK, .readlink = char_render129},
});
static struct proc_children dev_children = PROC_CHILDREN({
    {"char", S_IFDIR, .children = &dev_char_children},
});
static struct proc_children sys_root_children = PROC_CHILDREN({
    {"bus", S_IFDIR, .children = &bus_children},
    {"class", S_IFDIR, .children = &class_children},
    {"dev", S_IFDIR, .children = &dev_children},
    {"devices", S_IFDIR, .children = &devices_children},
});
struct proc_dir_entry sys_root = {NULL, S_IFDIR, .children = &sys_root_children};
