// Independent native oracle: Apple's libpcap classic BPF VM. No capture device
// is opened. Only deterministic in-memory packets and generated programs run.
#include <assert.h>
#include <stdio.h>
#include <pcap/bpf.h>
#include "fs/socket_filter.h"
static uint32_t state=0x73a9d51;
static uint32_t random_word(void){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
int main(void){
 _Static_assert(sizeof(struct bpf_insn)==sizeof(struct lr_bpf_insn),"BPF ABI");
 const uint16_t alu[]={0x04,0x14,0x24,0x34,0x44,0x54,0x64,0x74,0x94,0xa4};
 for(unsigned test=0;test<20000;test++){
  struct lr_bpf_insn p[10]={{0x20,0,0,random_word()%24},{0x01,0,0,random_word()%8},{alu[random_word()%10],0,0,random_word()%31+1},{0x02,0,0,0},{0x60,0,0,0},{0x15,0,1,random_word()},{0x04,0,0,random_word()},{0x07,0,0,0},{0x50,0,0,random_word()%4},{0x16,0,0,0}};
  if(test&1)p[8]=(struct lr_bpf_insn){0x16,0,0,0};
  else if(test&2)p[7]=(struct lr_bpf_insn){0x01,0,0,random_word()%32};
  const uint16_t jumps[]={0x15,0x25,0x35,0x45};p[5].code=jumps[test%4];p[5].k=(test&4)?random_word()%32:random_word();
  uint8_t packet[32];for(unsigned i=0;i<sizeof(packet);i++)packet[i]=random_word();size_t len=random_word()%33;
  assert(!lr_bpf_validate(p,10));assert(bpf_validate((const struct bpf_insn *)p,10));
  uint32_t expected=bpf_filter((const struct bpf_insn *)p,packet,len,len),actual=lr_bpf_run(p,10,packet,len);
  if(actual!=expected){fprintf(stderr,"BPF_ORACLE_FAIL case=%u actual=%u expected=%u\n",test,actual,expected);return 1;}
 }
 puts("SOCKET_FILTER_REFERENCE_PASS 20000 deterministic programs vs native libpcap; no packet capture");return 0;
}
