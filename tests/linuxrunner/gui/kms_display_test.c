// Host mailbox contract: cc -I. -pthread -fsanitize=address,undefined
// tests/linuxrunner/gui/kms_display_test.c -o /tmp/kms-display-test
#include <assert.h>
#include <stdio.h>
#include "fs/kms_display.inc"
static void *produce(void *unused) {
    (void)unused;
    uint32_t px[4] = {1,2,3,4};
    for (unsigned i=0; i<1000; i++) lr_kms_display_publish(px, 2, 2, 8);
    return NULL;
}
int main(void) {
    struct lr_kms_frame f = {0}; struct lr_kms_input e;
    _Static_assert(sizeof e == 32, "guest ioctl ABI");
    uint32_t padded[] = {0x00112233, 0x00445566, 0xdeadbeef, 0x00778899, 0x00aabbcc, 0xdeadbeef};
    lr_kms_display_publish(padded, 2, 2, 12); // no subscriber: no retained frame
    uint64_t a = lr_kms_display_begin();
    assert(lr_kms_display_pop(&e) && e.type == 3);
    assert(!lr_kms_display_take(a, &f));
    lr_kms_display_publish(padded, 2, 2, 12);
    padded[0] = 0; // snapshot must not alias the guest's reused buffer
    assert(lr_kms_display_take(a, &f) && f.width == 2 && f.height == 2);
    uint32_t *p = f.pixels;
    assert(p[0] == 0xff112233 && p[1] == 0xff445566 && p[2] == 0xff778899 && p[3] == 0xffaabbcc);
    free(f.pixels);
    lr_kms_display_publish(padded, 2, 2, 4); // invalid pitch
    lr_kms_display_publish(padded, 4096, 2, 16384); // viewer limit
    assert(!lr_kms_display_take(a, &f));
    lr_kms_display_publish(padded, 2, 2, 12);
    padded[0] = 7; lr_kms_display_publish(padded, 2, 2, 12);
    assert(lr_kms_display_take(a, &f) && ((uint32_t *)f.pixels)[0] == 0xff000007);
    free(f.pixels); assert(!lr_kms_display_take(a, &f));
    assert(lr_kms_display_input(a, (struct lr_kms_input){.type=1,.x=1}));
    assert(lr_kms_display_input(a, (struct lr_kms_input){.type=1,.x=2}));
    assert(lr_kms_display_input(a, (struct lr_kms_input){.type=1,.buttons=1}));
    assert(lr_kms_display_input(a, (struct lr_kms_input){.type=2,.keysym='a',.down=1}));
    assert(lr_kms_display_input(a, (struct lr_kms_input){.type=2,.keysym='a'}));
    assert(lr_kms_display_pop(&e) && e.x==2 && !e.buttons);
    assert(lr_kms_display_pop(&e) && e.buttons==1);
    assert(lr_kms_display_pop(&e) && e.keysym=='a' && e.down);
    assert(lr_kms_display_pop(&e) && e.keysym=='a' && !e.down);
    assert(!lr_kms_display_pop(&e));
    for (unsigned i=0; i<1024; i++) assert(lr_kms_display_input(a, (struct lr_kms_input){.type=2}));
    assert(!lr_kms_display_input(a, (struct lr_kms_input){.type=2}));
    uint64_t b = lr_kms_display_begin();
    lr_kms_display_end(a); // stale client cannot stop its replacement
    assert(!lr_kms_display_input(a, (struct lr_kms_input){.type=1}));
    assert(!lr_kms_display_take(a, &f));
    assert(lr_kms_display_pop(&e) && e.type==3 && !lr_kms_display_pop(&e));
    pthread_t thread; assert(!pthread_create(&thread, NULL, produce, NULL));
    for (unsigned i=0; i<1000; i++) if(lr_kms_display_take(b,&f)) free(f.pixels);
    pthread_join(thread,NULL);
    lr_kms_display_end(b);
    assert(!lr_kms_display_active() && !lr_kms_display_take(b,&f));
    assert(lr_kms_display_pop(&e) && e.type==3); // release stuck input on detach
    puts("KMS display mailbox PASS");
}
