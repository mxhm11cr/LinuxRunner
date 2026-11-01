#include <assert.h>
#include <stdio.h>
#include "fs/socket_filter.h"
int main(void){
 struct lr_bpf_insn p[]={{0x20,0,0,0},{0x15,0,1,0x12345678},{0x06,0,0,64},{0x06,0,0,0}};
 uint8_t matching[]={0x12,0x34,0x56,0x78},other[]={0x12,0x34,0x56,0x79};assert(!lr_bpf_validate(p,4));assert(lr_bpf_run(p,4,matching,4)==64&&lr_bpf_run(p,4,other,4)==0&&lr_bpf_run(p,4,matching,3)==0);
 struct lr_bpf_insn scratch[]={{0x00,0,0,4},{0x02,0,0,0},{0x60,0,0,0},{0x24,0,0,8},{0x16,0,0,0}};assert(!lr_bpf_validate(scratch,5)&&lr_bpf_run(scratch,5,NULL,0)==32);
 scratch[1].code=0x00;assert(lr_bpf_validate(scratch,5)==1);scratch[1]=(struct lr_bpf_insn){0x02,0,0,16};assert(lr_bpf_validate(scratch,5)==1);
 p[1].jt=10;assert(lr_bpf_validate(p,4)==1);p[1].jt=0;p[0].k=0xfffff000;assert(lr_bpf_validate(p,4)==2);
 struct lr_bpf_insn bad_div[]={{0x34,0,0,0},{0x06,0,0,10}};assert(lr_bpf_validate(bad_div,2)==1);
 struct lr_bpf_insn paths[]={{0x15,1,0,0},{0x02,0,0,0},{0x60,0,0,0},{0x06,0,0,1}};assert(lr_bpf_validate(paths,4)==1);
 struct lr_bpf_insn index[]={{0x01,0,0,0xffffffff},{0x40,0,0,5},{0x16,0,0,0}};assert(!lr_bpf_validate(index,3)&&lr_bpf_run(index,3,matching,4)==0);
 puts("SOCKET_FILTER_PASS bounds endian branches definite-scratch divide-by-zero unsupported-metadata");return 0;
}
