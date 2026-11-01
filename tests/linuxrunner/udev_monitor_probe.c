// Reproduce the prerequisite that currently prevents Arch KWin 6.8 startup.
// Exit 77 explicitly means unsupported, never successful KDE qualification.
#include <libudev.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <unistd.h>
int main(void) {
 int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC|SOCK_NONBLOCK,NETLINK_KOBJECT_UEVENT);int socket_error=errno;
 printf("UEVENT_SOCKET result=%d errno=%d\n",fd,fd<0?socket_error:0);if(fd>=0)close(fd);
 struct udev *udev=udev_new();if(!udev){perror("udev_new");return 1;}
 errno=0;struct udev_monitor *monitor=udev_monitor_new_from_netlink(udev,"udev");int monitor_error=errno;
 printf("UDEV_MONITOR present=%d errno=%d\n",monitor!=NULL,monitor_error);
 if(!monitor){udev_unref(udev);puts("BLOCKED: guest uevent service is not implemented; no fake monitor or device events supplied");return 77;}
 int result=udev_monitor_enable_receiving(monitor);printf("UDEV_MONITOR enable=%d fd=%d\n",result,udev_monitor_get_fd(monitor));udev_monitor_unref(monitor);udev_unref(udev);return result<0?1:0;
}
