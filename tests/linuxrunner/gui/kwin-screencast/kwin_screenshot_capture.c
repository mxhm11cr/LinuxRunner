// KWin's own screenshot of the workspace (org.kde.KWin.ScreenShot2
// CaptureWorkspace): raw pixels arrive through a pipe. Separates KWin's scene
// rendering from its screencast path. Usage: kwin_screenshot_capture OUT.raw
// KWin authorises the caller by its executable's desktop file
// (X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2).
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void add_bool(DBusMessageIter *dict, const char *key, dbus_bool_t value) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    DBusError err; dbus_error_init(&err);
    DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!bus) { printf("SHOT no bus: %s\n", err.message); return 3; }
    int p[2]; if (pipe(p)) return 4;
    DBusMessage *m = dbus_message_new_method_call("org.kde.KWin", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2", "CaptureWorkspace");
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(m, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
    add_bool(&dict, "native-resolution", TRUE);
    dbus_message_iter_close_container(&it, &dict);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_UNIX_FD, &p[1]);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, m, 30000, &err);
    close(p[1]);
    if (!reply) { printf("SHOT call failed: %s: %s\n", err.name, err.message); return 5; }
    unsigned width = 0, height = 0, stride = 0, format = 0;
    DBusMessageIter r, d; dbus_message_iter_init(reply, &r); dbus_message_iter_recurse(&r, &d);
    while (dbus_message_iter_get_arg_type(&d) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e, v; const char *key; dbus_message_iter_recurse(&d, &e);
        dbus_message_iter_get_basic(&e, &key); dbus_message_iter_next(&e); dbus_message_iter_recurse(&e, &v);
        if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT32) {
            dbus_uint32_t x; dbus_message_iter_get_basic(&v, &x);
            if (!strcmp(key, "width")) width = x; else if (!strcmp(key, "height")) height = x;
            else if (!strcmp(key, "stride")) stride = x; else if (!strcmp(key, "format")) format = x;
        }
        dbus_message_iter_next(&d);
    }
    size_t size = (size_t) stride * height, got = 0;
    unsigned char *buf = malloc(size ? size : 1);
    while (got < size) { ssize_t n = read(p[0], buf + got, size - got); if (n <= 0) break; got += (size_t) n; }
    unsigned maxv = 0; unsigned long long sum = 0, n = 0;
    for (unsigned y = 0; y < height; y += 4) for (unsigned x = 0; x < width; x += 4) {
        unsigned char *px = buf + (size_t) y * stride + x * 4; unsigned v = px[0] > px[1] ? px[0] : px[1]; v = v > px[2] ? v : px[2];
        if (v > maxv) maxv = v; sum += v; n++;
    }
    printf("SHOT %ux%u stride=%u format=%u bytes=%zu/%zu max=%u mean=%.2f\n", width, height, stride, format, got, size, maxv, n ? (double) sum / n : 0);
    FILE *f = fopen(argv[1], "wb"); if (f) { fwrite(buf, 1, got, f); fclose(f); }
    return got == size && size ? 0 : 6;
}
