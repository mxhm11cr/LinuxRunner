#include <assert.h>
#include "app/LinuxRunner/JITActivationPolicy.h"
int main(void) {
    for (unsigned os=0;os<2;os++) for(unsigned debugger=0;debugger<2;debugger++)
    for(unsigned classic=0;classic<2;classic++) for(unsigned prepared=0;prepared<2;prepared++) {
        unsigned paths=lr_jit_activation_paths(os,debugger,classic,prepared);
        assert(!!(paths&LRJITTryClassic)==(!os&&!classic));
        assert(!!(paths&LRJITTryPrepared)==(os&&debugger&&!prepared));
        assert(!(paths&~(LRJITTryClassic|LRJITTryPrepared)));
    }
    // A detached Classic authorization may still be valid on iOS 18.
    assert(lr_jit_activation_paths(false,false,false,false)==LRJITTryClassic);
    // A quarantined Classic method never retries automatically on iOS 18.
    assert(lr_jit_activation_paths(false,false,true,false)==0);
    // iOS 26+ requires preparation; no Classic executable self-test first.
    assert(lr_jit_activation_paths(true,false,false,false)==0);
    assert(lr_jit_activation_paths(true,true,false,false)==LRJITTryPrepared);
    return 0;
}
