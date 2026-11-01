// Host contract for the production proc formatter. No guest tasks required.
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "fs/proc/net.c"
int errno_map(void) { return -errno; }
void proc_printf(struct proc_data *out, const char *format, ...) {
    va_list ap; va_start(ap, format);
    char *text = NULL; int n = vasprintf(&text, format, ap); va_end(ap);
    assert(n >= 0);
    out->data = realloc(out->data, out->size + n + 1); assert(out->data);
    memcpy(out->data + out->size, text, n + 1); out->size += n; free(text);
}
int main(void) {
    struct proc_data data = {0};
    assert(proc_show_net_dev(NULL, &data) == 0);
    assert(strstr(data.data, "Receive") && strstr(data.data, "Transmit"));
    char *save = NULL; unsigned interfaces = 0;
    for (char *line = strtok_r(data.data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':'); if (!colon) continue;
        *colon = 0; char name[IF_NAMESIZE]; assert(sscanf(line, "%15s", name) == 1);
        assert(if_nametoindex(name)); // Every exposed name really exists.
        unsigned count = 0; char *end = colon + 1;
        while (*end) { char *next; strtoull(end, &next, 10); if (next == end) break; count++; end = next; }
        assert(count == 16); interfaces++;
    }
    assert(interfaces > 0); free(data.data);
    puts("PROC_NET_DEV_PASS real interfaces, 16 counters, no fabricated hardware");
}
