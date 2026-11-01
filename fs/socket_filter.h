// Classic socket BPF for guest event datagrams. No host packet access.
#ifndef LR_SOCKET_FILTER_H
#define LR_SOCKET_FILTER_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
struct lr_bpf_insn { uint16_t code; uint8_t jt,jf; uint32_t k; };
// 0 = valid, 1 = malformed, 2 = valid instruction family not implemented.
static int lr_bpf_validate(const struct lr_bpf_insn *p,size_t n) {
    if(!n||n>4096)return 1;
    if(p[n-1].code!=0x06&&p[n-1].code!=0x16)return 1;
    uint16_t *defined=calloc(n,sizeof(*defined));bool *seen=calloc(n,sizeof(*seen));
    if(!defined||!seen){free(defined);free(seen);return 3;}seen[0]=true;
    int error=0;
    for(size_t i=0;i<n&&!error;i++) {
        uint16_t code=p[i].code;uint32_t k=p[i].k;size_t target[2]={i+1,0};unsigned edges=1;uint16_t mask=defined[i];
        switch(code) {
            case 0x00:case 0x01:case 0x80:case 0x81:break; // IMM, LEN
            case 0x20:case 0x28:case 0x30:case 0xb1:
                if(k>=0xfffff000U)error=2;break; // ancillary packet metadata unavailable
            case 0x40:case 0x48:case 0x50:break; // IND
            case 0x60:case 0x61: // MEM reads require a store on every incoming path
                if(k>=16||(seen[i]&&!(mask&(1U<<k))))error=1;break;
            case 0x02:case 0x03:
                if(k>=16)error=1;else mask|=1U<<k;break;
            case 0x04:case 0x0c:case 0x14:case 0x1c:case 0x24:case 0x2c:
            case 0x3c:case 0x44:case 0x4c:case 0x54:case 0x5c:case 0x84:
            case 0x9c:case 0xa4:case 0xac:break;
            case 0x34:case 0x94:if(!k)error=1;break;
            case 0x64:case 0x74:if(k>=32)error=1;break;
            case 0x6c:case 0x7c:error=2;break; // dynamic shift semantics not promised
            case 0x05:if(k>=n-i-1)error=1;else target[0]=i+1+k;break;
            case 0x15:case 0x1d:case 0x25:case 0x2d:case 0x35:case 0x3d:case 0x45:case 0x4d:
                if(p[i].jt>=n-i-1||p[i].jf>=n-i-1)error=1;
                else{target[0]=i+1+p[i].jt;target[1]=i+1+p[i].jf;edges=2;}break;
            case 0x06:case 0x16:edges=0;break;
            case 0x07:case 0x87:break;
            default:error=1;break;
        }
        if(seen[i])for(unsigned j=0;j<edges&&!error;j++){
            size_t t=target[j];if(t>=n){error=1;break;}
            if(seen[t])defined[t]&=mask;else{defined[t]=mask;seen[t]=true;}
        }
    }
    free(defined);free(seen);return error;
}
static uint32_t lr_bpf_run(const struct lr_bpf_insn *p,size_t n,const uint8_t *bytes,size_t size) {
    if(!p)return UINT32_MAX;
    uint32_t a=0,x=0,m[16]={0};
    for(size_t i=0;i<n;i++){
        uint16_t code=p[i].code;uint32_t k=p[i].k,v=(code&8)?x:k;
        if((code&7)==0||(code&7)==1){
            uint32_t value=0;unsigned mode=code&0xe0;
            if(mode==0)value=k;
            else if(mode==0x80)value=size;
            else if(mode==0x60){if(k>=16)return 0;value=m[k];}
            else {
                uint64_t offset=(uint64_t)k+(mode==0x40?x:0);unsigned width=(code&0x18)==0?4:(code&0x18)==8?2:1;
                if(offset>size||width>size-offset)return 0;
                for(unsigned j=0;j<width;j++)value=(value<<8)|bytes[offset+j];
                if(mode==0xa0)value=(value&15)*4;
            }
            if((code&7)==1)x=value;else a=value;
        }else switch(code&7){
            case 2:if(k>=16){return 0;}m[k]=a;break;
            case 3:if(k>=16){return 0;}m[k]=x;break;
            case 4:switch(code&0xf0){
                case 0x00:a+=v;break;case 0x10:a-=v;break;case 0x20:a*=v;break;
                case 0x30:if(!v)return 0;a/=v;break;case 0x40:a|=v;break;case 0x50:a&=v;break;
                case 0x60:if(v>=32)return 0;a<<=v;break;case 0x70:if(v>=32)return 0;a>>=v;break;
                case 0x80:a=0U-a;break;case 0x90:if(!v)return 0;a%=v;break;case 0xa0:a^=v;break;
                default:return 0;}break;
            case 5:switch(code&0xf0){
                case 0:i+=k;break;case 0x10:i+=a==v?p[i].jt:p[i].jf;break;
                case 0x20:i+=a>v?p[i].jt:p[i].jf;break;case 0x30:i+=a>=v?p[i].jt:p[i].jf;break;
                case 0x40:i+=(a&v)?p[i].jt:p[i].jf;break;default:return 0;}break;
            case 6:return code==0x16?a:k;
            case 7:if(code==0x07)x=a;else a=x;break;
            default:return 0;
        }
    }return 0;
}
#endif
