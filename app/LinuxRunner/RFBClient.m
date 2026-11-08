#import "RFBClient.h"
#import "FramebufferPatch.h"
#import "RFBProtocol.h"
#include <CommonCrypto/CommonCryptor.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <time.h>
#include <os/lock.h>
static double LRTransportClock(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
@implementation LRRFBClient {
    uint16_t _port,_width,_height;
    NSString *_password,*_failure;
    int _socket;
    pthread_mutex_t _socketLock,_writeLock;
    _Atomic bool _stopped,_started,_ready;
    _Atomic unsigned _pending;
    _Atomic size_t _pendingBytes;
    _Atomic uint64_t _inputSequence;
    dispatch_queue_t _inputQueue;
    os_unfair_lock _motionLock; // guards _openMotion
    NSMutableData *_openMotion; // newest queued, unwritten pure-motion pointer packet
    uint8_t _lastButtons; // button mask of the last queued pointer packet
    _Atomic uint64_t _coalescedMotion;
    NSMutableData *_pixels,*_raw;
    uint8_t _readBuffer[65536]; size_t _readOffset,_readCount;
    uint64_t _frames,_bytes;
}
- (instancetype)initWithPort:(uint16_t)port password:(NSString *)password {
    if((self=[super init])){_port=port;_password=[password copy];_socket=-1;
        pthread_mutex_init(&_socketLock,NULL);pthread_mutex_init(&_writeLock,NULL);_motionLock=OS_UNFAIR_LOCK_INIT;
        _inputQueue=dispatch_queue_create("dev.linuxrunner.gui.input",DISPATCH_QUEUE_SERIAL);
    }return self;
}
- (void)dealloc {pthread_mutex_destroy(&_socketLock);pthread_mutex_destroy(&_writeLock);}
- (void)status:(NSString *)message {void (^handler)(NSString *)=self.statusHandler;if(handler)handler(message);}
- (void)fail:(NSString *)reason {_failure=[@"Display error: " stringByAppendingString:reason];[self status:_failure];}
- (void)start {
    if(atomic_exchange(&_started,true))return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool{
        [self status:@"Waiting for Linux display…"];
        BOOL connected=NO;
        for(unsigned attempt=0;attempt<300&&!atomic_load(&self->_stopped);attempt++){
            int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)break;
            int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
            setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
            struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
            struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(self->_port)};addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            if(connect(fd,(struct sockaddr *)&addr,sizeof addr)==0){
                pthread_mutex_lock(&self->_socketLock);
                if(!atomic_load(&self->_stopped)){self->_socket=fd;connected=YES;}else close(fd);
                pthread_mutex_unlock(&self->_socketLock);break;
            }close(fd);usleep(100000);
        }
        if(connected&&[self handshake]){atomic_store(&self->_ready,true);[self status:@"Connected to real Linux X11 display"];
            if([self request:NO])while(!atomic_load(&self->_stopped)){
                BOOL more;
                // A desktop can remain connected for hours. Release decoder
                // and callback temporaries per update, not only at disconnect.
                // Retained frame snapshots and pending input keep their owners.
                @autoreleasepool {more=[self update];}
                if(!more)break;
            }
        }else if(!atomic_load(&self->_stopped)&&!self->_failure)[self fail:@"Could not connect to the Linux display."];
        atomic_store(&self->_ready,false);
        BOOL wasStopped=atomic_exchange(&self->_stopped,true);
        pthread_mutex_lock(&self->_socketLock);pthread_mutex_lock(&self->_writeLock);
        if(self->_socket>=0){shutdown(self->_socket,SHUT_RDWR);close(self->_socket);self->_socket=-1;}
        pthread_mutex_unlock(&self->_writeLock);pthread_mutex_unlock(&self->_socketLock);
        if(connected&&!wasStopped&&!self->_failure)[self status:@"Linux display ended. CLI runtime remains available."];
    }});
}
- (void)stop {
    atomic_store(&_stopped,true);atomic_store(&_ready,false);
    pthread_mutex_lock(&_socketLock);if(_socket>=0)shutdown(_socket,SHUT_RDWR);pthread_mutex_unlock(&_socketLock);
}
- (BOOL)read:(void *)buffer count:(size_t)count {
    uint8_t *p=buffer;
    while(count&&!atomic_load(&_stopped)){
        if(_readOffset==_readCount){
            // Blocking receive sleeps without 100ms polling wakeups. stop()
            // uses shutdown(), which interrupts the sole reader immediately.
            ssize_t n=recv(_socket,_readBuffer,sizeof _readBuffer,0);if(n<=0){if(n<0&&errno==EINTR)continue;return NO;}
            _readOffset=0;_readCount=(size_t)n;_bytes+=(uint64_t)n;
        }
        size_t part=MIN(count,_readCount-_readOffset);memcpy(p,_readBuffer+_readOffset,part);
        p+=part;count-=part;_readOffset+=part;
    }return count==0;
}
- (BOOL)write:(const void *)buffer count:(size_t)count {
    pthread_mutex_lock(&_writeLock);const uint8_t *p=buffer;
    BOOL ok=YES;
    while(count){if(atomic_load(&_stopped)){ok=NO;break;}
        ssize_t n=send(_socket,p,count,0);if(n<=0){if(n<0&&errno==EINTR)continue;ok=NO;break;}
        p+=n;count-=n;
    }pthread_mutex_unlock(&_writeLock);return ok;
}
- (BOOL)handshake {
    uint8_t version[12];if(![self read:version count:12])return NO;
    if(memcmp(version,"RFB 003.008\n",12)&&memcmp(version,"RFB 003.007\n",12)){[self fail:@"Unsupported RFB version"];return NO;}
    if(![self write:version count:12])return NO;
    uint8_t n;if(![self read:&n count:1]||!n)return NO;
    uint8_t types[255];if(![self read:types count:n])return NO;
    BOOL auth=NO;for(unsigned i=0;i<n;i++)if(types[i]==2)auth=YES;
    if(!auth){[self fail:@"Session authentication required"];return NO;}
    uint8_t selected=2,challenge[16],response[16],key[8]={0};
    if(![self write:&selected count:1]||![self read:challenge count:16])return NO;
    NSData *password=[_password dataUsingEncoding:NSASCIIStringEncoding];if(password.length!=8)return NO;
    for(unsigned i=0;i<8;i++)key[i]=lr_rfb_reverse_bits(((const uint8_t *)password.bytes)[i]);
    size_t out=0;
    CCCryptorStatus crypt=CCCrypt(kCCEncrypt,kCCAlgorithmDES,kCCOptionECBMode,key,8,NULL,challenge,16,response,16,&out);
    memset(key,0,sizeof key);_password=nil;
    if(crypt!=kCCSuccess||out!=16||![self write:response count:16])return NO;
    uint32_t security;if(![self read:&security count:4]||ntohl(security)!=0){[self fail:@"Authentication failed"];return NO;}
    uint8_t shared=0;if(![self write:&shared count:1])return NO;
    uint8_t init[24];if(![self read:init count:24])return NO;
    uint16_t w,h;uint32_t nameLength;memcpy(&w,init,2);memcpy(&h,init+2,2);memcpy(&nameLength,init+20,4);
    _width=ntohs(w);_height=ntohs(h);size_t bytes;
    if(!lr_rfb_dimensions(_width,_height,&bytes)||ntohl(nameLength)>4096){[self fail:@"Dimensions/name exceed limits"];return NO;}
    uint8_t name[4096];if(![self read:name count:ntohl(nameLength)])return NO;
    _pixels=[NSMutableData dataWithLength:bytes];_raw=[NSMutableData new];
    // SetPixelFormat: little-endian 32-bit truecolor B,G,R,pad.
    const uint8_t format[20]={0,0,0,0,32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0};
    // Hextile (bounded 16x16 tiles), Raw fallback, DesktopSize, Cursor and
    // PointerPos (the app draws the pointer: x11vnc does not repaint a cursor
    // drawn into the framebuffer when this viewer itself moves the pointer).
    const uint8_t encodings[24]={2,0,0,5,0,0,0,5,0,0,0,0,255,255,255,33,255,255,255,17,255,255,255,24};
    return [self write:format count:sizeof format]&&[self write:encodings count:sizeof encodings];
}
- (BOOL)request:(BOOL)incremental {
    uint8_t packet[10]={3,incremental?1:0,0,0,0,0,(uint8_t)(_width>>8),(uint8_t)_width,(uint8_t)(_height>>8),(uint8_t)_height};
    return [self write:packet count:sizeof packet];
}
- (BOOL)update {
    uint8_t type;if(![self read:&type count:1])return NO;
    if(type==2)return YES; // bell, no payload
    if(type==3){uint8_t header[7];if(![self read:header count:7])return NO;
        uint32_t n;memcpy(&n,header+3,4);n=ntohl(n);if(n>1024*1024)return NO;
        NSMutableData *text=[NSMutableData dataWithLength:n];
        if(n&&![self read:text.mutableBytes count:n])return NO;
        void (^cut)(NSString *)=self.cutTextHandler;
        NSString *string=[[NSString alloc] initWithData:text encoding:NSISOLatin1StringEncoding];
        if(cut&&string)cut(string);
        return YES;
    }
    if(type!=0){[self fail:@"Unsupported display message"];return NO;}
    void (^metrics)(NSDictionary *)=self.transportMetricsHandler;
    double decodeBegin=metrics?LRTransportClock():0;
    uint8_t header[3];if(![self read:header count:3])return NO;
    uint16_t rects;memcpy(&rects,header+1,2);rects=ntohs(rects);if(rects>4096){[self fail:@"Rectangle count exceeds limit"];return NO;}
    BOOL changed=NO; LRFrameDamage damage={0};
    for(unsigned i=0;i<rects;i++){
        uint8_t rh[12];if(![self read:rh count:12])return NO;
        uint16_t x,y,w,h;uint32_t encoding;
        memcpy(&x,rh,2);memcpy(&y,rh+2,2);memcpy(&w,rh+4,2);memcpy(&h,rh+6,2);memcpy(&encoding,rh+8,4);
        x=ntohs(x);y=ntohs(y);w=ntohs(w);h=ntohs(h);encoding=ntohl(encoding);
        size_t bytes;
        if(encoding==0xffffff21){if(x||y||!lr_rfb_dimensions(w,h,&bytes))return NO;
            _width=w;_height=h;_pixels=[NSMutableData dataWithLength:bytes];changed=YES;damage=(LRFrameDamage){0,0,w,h};continue;}
        if(encoding==0xffffff11){size_t pb,mb;
            if(!lr_rfb_cursor_sizes(w,h,&pb,&mb)||(w&&(x>=w||y>=h))){[self fail:@"Invalid cursor shape"];return NO;}
            NSMutableData *shape=[NSMutableData dataWithLength:pb+mb],*bgra=nil;
            if(pb+mb&&![self read:shape.mutableBytes count:pb+mb])return NO;
            if(w&&h){bgra=[NSMutableData dataWithLength:pb];
                if(!lr_rfb_cursor_bgra(shape.bytes,(const uint8_t *)shape.bytes+pb,w,h,bgra.mutableBytes)){[self fail:@"Invalid cursor shape"];return NO;}}
            void (^cursor)(NSData *,uint16_t,uint16_t,uint16_t,uint16_t)=self.cursorHandler;
            if(cursor)cursor(bgra,w,h,x,y);
            continue;}
        if(encoding==0xffffff18){void (^moved)(uint16_t,uint16_t)=self.pointerPositionHandler;
            if(moved&&x<_width&&y<_height)moved(x,y);
            continue;}
        if((encoding!=0&&encoding!=5)||!lr_rfb_rectangle(_width,_height,x,y,w,h,&bytes)){[self fail:@"Unsupported encoding or invalid rectangle"];return NO;}
        if(encoding==5){if(![self hextileX:x y:y width:w height:h]){[self fail:@"Invalid Hextile tile"];return NO;}}
        else if(!x&&w==_width){
            // Full-width rows are contiguous in the owned framebuffer. Receive
            // there directly instead of allocating/copying a second rectangle.
            // A failed read ends the session before any partial frame is sent.
            if(![self read:(uint8_t *)_pixels.mutableBytes+(size_t)y*_width*4 count:bytes])return NO;
        }else{[_raw setLength:bytes];if(![self read:_raw.mutableBytes count:bytes])return NO;
            if(bytes&&!lr_rfb_apply_raw(_pixels.mutableBytes,_width,_height,x,y,w,h,_raw.bytes,bytes))return NO;}
        changed|=bytes!=0; if(bytes)damage=lr_damage_union(damage,(LRFrameDamage){x,y,w,h});
    }
    if(changed){_frames++;
        if(_frames==1)damage=(LRFrameDamage){0,0,_width,_height};
        // Disconnect can clear callback properties on another queue. Retain
        // each selected callback once, then test and invoke that same value.
        void (^patchHandler)(NSData *,uint16_t,uint16_t,uint64_t,uint64_t,LRFrameDamage)=self.patchFrameHandler;
        if(patchHandler) {
            NSData *patch=LRFramebufferPatch(_pixels,_width,_height,damage);
            if(!patch){[self fail:@"Invalid framebuffer patch"];return NO;}
            patchHandler(patch,_width,_height,_frames,_bytes,damage);
        } else {
            void (^damageHandler)(NSData *,uint16_t,uint16_t,uint64_t,uint64_t,LRFrameDamage)=self.damageFrameHandler;
            void (^frameHandler)(NSData *,uint16_t,uint16_t,uint64_t,uint64_t)=self.frameHandler;
            if(damageHandler||frameHandler){
                NSData *snapshot=_pixels.copy;
                if(damageHandler)damageHandler(snapshot,_width,_height,_frames,_bytes,damage);
                else frameHandler(snapshot,_width,_height,_frames,_bytes);
            }
        }
    }
    if(metrics&&changed)metrics(@{@"stage":@"rfb_decode_delivery",@"frame":@(_frames),
        @"rectangles":@(rects),@"damage_bytes":@((uint64_t)damage.width*damage.height*4),
        @"full_frame_bytes":@((uint64_t)_width*_height*4),@"received_bytes":@(_bytes),
        @"decode_delivery_ms":@((LRTransportClock()-decodeBegin)*1000)});
    // The server blocks an incremental request until dirty pixels exist. Do not
    // add another 80ms round-trip wait to every interaction.
    return [self request:YES];
}
- (BOOL)hextileX:(uint16_t)x y:(uint16_t)y width:(uint16_t)w height:(uint16_t)h {
    uint8_t bg[4]={0},fg[4]={0},raw[1024];BOOL hasBG=NO,hasFG=NO;
    for(unsigned ty=0;ty<h;ty+=16)for(unsigned tx=0;tx<w;tx+=16){
        unsigned tw=MIN(16,w-tx),th=MIN(16,h-ty);uint8_t flags;
        if(![self read:&flags count:1]||(flags&~31))return NO;
        if(flags&1){size_t bytes=tw*th*4;if(![self read:raw count:bytes]||!lr_rfb_apply_raw(_pixels.mutableBytes,_width,_height,x+tx,y+ty,tw,th,raw,bytes))return NO;continue;}
        if(flags&2){if(![self read:bg count:4])return NO;hasBG=YES;}
        if(flags&4){if(![self read:fg count:4])return NO;hasFG=YES;}
        if(!hasBG||!lr_rfb_fill(_pixels.mutableBytes,_width,_height,x+tx,y+ty,tw,th,bg))return NO;
        if(flags&8){uint8_t count;if(![self read:&count count:1])return NO;
            for(unsigned i=0;i<count;i++){uint8_t color[4],rect[2];
                if(flags&16){if(![self read:color count:4])return NO;}else{if(!hasFG)return NO;memcpy(color,fg,4);}
                if(![self read:rect count:2])return NO;
                unsigned sx=rect[0]>>4,sy=rect[0]&15,sw=(rect[1]>>4)+1,sh=(rect[1]&15)+1;
                if(sx+sw>tw||sy+sh>th||!lr_rfb_fill(_pixels.mutableBytes,_width,_height,x+tx+sx,y+ty+sy,sw,sh,color))return NO;
            }
        }else if(flags&16)return NO;
    }return YES;
}
// Charge the retained input data, not serialized bytes, against a bounded
// 128 KiB queue. A paste owns one job and is encoded in 1 KiB stack batches.
// Keep every key pair and subsequent pointer/key event in serial FIFO order.
- (void)enqueue:(NSData *)data keySymbols:(BOOL)keySymbols { [self enqueue:data keySymbols:keySymbols motion:NO]; }
// A motion packet stays open for coalescing until it is written or any later
// packet is queued, so updates never cross button, key or other events.
- (void)enqueue:(NSData *)data keySymbols:(BOOL)keySymbols motion:(BOOL)motion {
    if(!data.length||!atomic_load(&_ready)||atomic_load(&_stopped))return;
    const size_t limit=128*1024;
    BOOL slotsFull=atomic_fetch_add(&_pending,1)>=128;
    if(slotsFull){atomic_fetch_sub(&_pending,1);[self fail:@"Input queue limit exceeded; close/relaunch the display."];[self stop];return;}
    size_t previous=atomic_fetch_add(&_pendingBytes,data.length);
    if(data.length>limit||previous>limit-data.length){
        atomic_fetch_sub(&_pendingBytes,data.length);atomic_fetch_sub(&_pending,1);
        [self fail:@"Input byte queue limit exceeded; close/relaunch the display."];[self stop];return;
    }
    void (^metrics)(NSDictionary *)=self.transportMetricsHandler;
    double queued=metrics?LRTransportClock():0;
    uint64_t sequence=metrics?atomic_fetch_add(&_inputSequence,1)+1:0;
    unsigned pending=metrics?atomic_load(&_pending):0;
    size_t length=data.length;os_unfair_lock_lock(&_motionLock);_openMotion=motion?(NSMutableData *)data:nil;
    dispatch_async(_inputQueue,^{@autoreleasepool{
        double began=metrics?LRTransportClock():0;BOOL ok=YES;
        void (^trace)(NSString *)=self.inputTraceHandler;
        size_t wireBytes=keySymbols?length*4:length;
        if(keySymbols){
            const uint32_t *symbols=data.bytes;size_t count=length/4;
            for(size_t offset=0;offset<count;offset+=64){
                size_t n=MIN((size_t)64,count-offset);uint8_t packets[1024];
                for(size_t i=0;i<n;i++){uint32_t sym=symbols[offset+i];
                    uint8_t key[8]={4,1,0,0,(uint8_t)(sym>>24),(uint8_t)(sym>>16),(uint8_t)(sym>>8),(uint8_t)sym};
                    memcpy(packets+i*16,key,8);key[1]=0;memcpy(packets+i*16+8,key,8);
                }
                if(![self write:packets count:n*16]){ok=NO;break;}
                if(trace)for(size_t i=0;i<n;i++){
                    uint32_t sym=symbols[offset+i];
                    trace([NSString stringWithFormat:@"RFB keysym=0x%08X down=1 written=%d",sym,ok]);
                    trace([NSString stringWithFormat:@"RFB keysym=0x%08X down=0 written=%d",sym,ok]);
                }
            }
        }else if(motion){
            uint8_t packet[6];os_unfair_lock_lock(&self->_motionLock);
            if(self->_openMotion==data)self->_openMotion=nil;memcpy(packet,data.bytes,sizeof packet);
            os_unfair_lock_unlock(&self->_motionLock);ok=[self write:packet count:sizeof packet];
        }else{
            ok=[self write:data.bytes count:length];
            if(length%8==0&&((const uint8_t*)data.bytes)[0]==4&&trace){
                for(size_t i=0;i<length;i+=8){const uint8_t *p=(const uint8_t*)data.bytes+i;uint32_t sym=(uint32_t)p[4]<<24|(uint32_t)p[5]<<16|(uint32_t)p[6]<<8|p[7];trace([NSString stringWithFormat:@"RFB keysym=0x%08X down=%u written=%d",sym,p[1],ok]);}
            }
        }
        if(metrics)metrics(@{@"stage":@"rfb_input_write",@"packet_type":@(keySymbols?4:motion?5:((const uint8_t *)data.bytes)[0]),
            @"packet_bytes":@(wireBytes),@"queue_depth_at_enqueue":@(pending),@"written":@(ok),
            @"input_sequence":@(sequence),@"coalesced_motion":@(atomic_load(&self->_coalescedMotion)),@"queued_monotonic_ms":@(queued*1000),@"write_begin_monotonic_ms":@(began*1000),
            @"input_queue_ms":@((began-queued)*1000),@"write_ms":@((LRTransportClock()-began)*1000)});
        atomic_fetch_sub(&self->_pendingBytes,length);atomic_fetch_sub(&self->_pending,1);
    }});
    os_unfair_lock_unlock(&_motionLock);
}
- (void)enqueue:(NSData *)data { [self enqueue:data keySymbols:NO]; }
- (void)key:(uint32_t)keysym down:(BOOL)down {
    uint8_t data[8]={4,down?1:0,0,0,(uint8_t)(keysym>>24),(uint8_t)(keysym>>16),(uint8_t)(keysym>>8),(uint8_t)keysym};
    [self enqueue:[NSData dataWithBytes:data length:sizeof data]];
}
// Bound each retained symbol block at 64 KiB. The GUI's 16 KiB text limit
// fits one job; encoding stays bounded at 64 characters / 1 KiB per write.
- (void)typeKeysyms:(const uint32_t *)symbols count:(size_t)count {
    if(!symbols)return;
    for(size_t offset=0;offset<count;){size_t n=MIN((size_t)16384,count-offset);
        [self enqueue:[NSData dataWithBytes:symbols+offset length:n*sizeof(*symbols)] keySymbols:YES];
        if(atomic_load(&_stopped))break;offset+=n;
    }
}
- (void)pointerX:(uint16_t)x y:(uint16_t)y buttons:(uint8_t)buttons {
    uint8_t data[6]={5,buttons,(uint8_t)(x>>8),(uint8_t)x,(uint8_t)(y>>8),(uint8_t)y};
    // RFB pointer positions are absolute: replacing an unwritten move with the
    // same button mask keeps the final position and drops only stale samples.
    // Button transitions (press, release, wheel) are never opened or replaced.
    os_unfair_lock_lock(&_motionLock);NSMutableData *open=_openMotion;
    BOOL transition=buttons!=_lastButtons;_lastButtons=buttons;
    if(!transition&&open){memcpy(open.mutableBytes,data,sizeof data);os_unfair_lock_unlock(&_motionLock);atomic_fetch_add(&_coalescedMotion,1);return;}
    os_unfair_lock_unlock(&_motionLock);
    [self enqueue:[NSMutableData dataWithBytes:data length:sizeof data] keySymbols:NO motion:!transition];
}
@end
