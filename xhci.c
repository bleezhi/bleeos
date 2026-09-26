/* Minimal xHCI host controller + USB2 HID support.
 * Polled, no interrupts/hotplug/hubs. DMA structures live in the
 * low kernel image so this 32-bit kernel can hand them to xHCI. */
#include "xhci.h"
#include "pci.h"
#include "drivers.h"
#include "heap.h"
typedef unsigned long long u64;

#define XCAP_CAPL 0x00
#define XCAP_HCSP1 0x04
#define XCAP_HCC1 0x10
#define XCAP_DBOFF 0x14
#define XCAP_RTSOFF 0x18

#define XOP_CMD 0x00
#define XOP_STS 0x04
#define XOP_PAGESZ 0x08
#define XOP_CRCR 0x18
#define XOP_DCBAAP 0x30
#define XOP_CONFIG 0x38
#define XOP_PORTS 0x400

#define CMD_RS 0x00000001
#define CMD_HCRST 0x00000002
#define STS_HCH 0x00000001
#define STS_CNR 0x00000800

#define PORT_CCS 0x00000001
#define PORT_PED 0x00000002
#define PORT_PR  0x00000010
#define PORT_PP  0x00000200
#define PORT_SPEED(x) (((x)>>10)&15)
#define PORT_PRC 0x00200000

#define TRB_CYCLE 1u
#define TRB_CHAIN (1u<<4)
#define TRB_IOC (1u<<5)
#define TRB_IDT (1u<<6)
#define TRB_DIR_IN (1u<<16)
#define TRB_TRT_OUT (2u<<16)
#define TRB_TRT_IN (3u<<16)
#define TRB_TYPE(x) ((u32)(x)<<10)
#define TRB_LINK_TOGGLE 2u
#define TRB_ENABLE_SLOT TRB_TYPE(9)
#define TRB_ADDR_DEV TRB_TYPE(11)
#define TRB_CONFIG_EP TRB_TYPE(12)
#define TRB_EVAL_CONTEXT TRB_TYPE(13)
#define TRB_SETUP TRB_TYPE(2)
#define TRB_DATA TRB_TYPE(3)
#define TRB_STATUS TRB_TYPE(4)
#define TRB_NORMAL TRB_TYPE(1)
#define TRB_COMPLETION 33
#define TRB_TRANSFER 32

#define COMP_SUCCESS 1
#define COMP_SHORT 13

typedef struct { u32 a,b,c,d; } trb_t;

static volatile u8 *mmio;
static u32 op, db, rt;
static int ready, maxports, maxslots, ctx_size=32;
static int sp_count;
static u64 *sp_table;
static void *sp_pages[8];
static int cmd_i, cmd_cycle=1;
static int ev_i, ev_cycle=1;
static u32 ev_dequeue;

__attribute__((aligned(64))) static u64 dcbaa[256];
__attribute__((aligned(64))) static trb_t cmd_ring[32];
__attribute__((aligned(64))) static trb_t event_ring[64];
__attribute__((aligned(64))) static u64 erst[2];
__attribute__((aligned(64))) static u8 out_ctx[2][4096];
__attribute__((aligned(64))) static u8 in_ctx[4096];
__attribute__((aligned(64))) static trb_t ctrl_rings[2][32];
__attribute__((aligned(64))) static trb_t kbd_rings[2][32];
__attribute__((aligned(64))) static trb_t mouse_rings[2][32];

typedef struct {
    int used, index, port, slot, speed;
    u8 mps, kbd_ep, mouse_ep, kbd_mps, mouse_mps;
    int kbd_toggle, mouse_toggle;
    u8 prev[6], mod;
    int ep_i, ep_cycle;
    int k_i,k_cycle,m_i,m_cycle;
} xdev_t;
static xdev_t devs[2];
static int ndev;
static u8 kbuf[2][8], mbuf[2][4];
static int k_pending[2], m_pending[2];

static inline u32 rr(u32 o){ return *(volatile u32 *)(mmio+o); }
static inline void rw(u32 o,u32 v){ *(volatile u32 *)(mmio+o)=v; }
static void zero(void *p,u32 n){u8 *q=p;while(n--)*q++=0;}
static u64 ptr64(const void *p){return (u64)(u32)p;}
static u32 *ctx(u8 *base,int idx){return (u32 *)(base+idx*ctx_size);}

static int wait32(u32 off,u32 mask,u32 want,u32 loops){
    while(loops--) if((rr(off)&mask)==want) return 0;
    return -1;
}
static void trb_clear(trb_t *t){t->a=t->b=t->c=t->d=0;}
static void ring_init(void){
    for(int i=0;i<32;i++)trb_clear(&cmd_ring[i]);
    cmd_ring[31].a=(u32)ptr64(cmd_ring);
    cmd_ring[31].d=TRB_TYPE(6)|TRB_LINK_TOGGLE|1;
    for(int i=0;i<64;i++)trb_clear(&event_ring[i]);
    cmd_i=0;cmd_cycle=1;ev_i=0;ev_cycle=1;
}
static void cmd_put(u32 a,u32 b,u32 c,u32 d){
    trb_t *t=&cmd_ring[cmd_i];
    t->a=a;t->b=b;t->c=c;t->d=(d&~1u)|(u32)cmd_cycle;
    cmd_i++;
    if(cmd_i==31){cmd_ring[31].d=TRB_TYPE(6)|TRB_LINK_TOGGLE|(u32)cmd_cycle;cmd_i=0;cmd_cycle^=1;}
    rw(db,0);
}
static int event_wait(u32 type,u32 *slot,u32 *status,u32 timeout){
    while(timeout--){
        trb_t *e=&event_ring[ev_i];
        if((e->d&1u)!=((u32)ev_cycle)) { sleep_ms(1); continue; }
        u32 et=(e->d>>10)&63;
        u32 st=e->c;
        u32 sl=e->d>>24;
        ev_i++;
        if(ev_i==64){ev_i=0;ev_cycle^=1;}
        ev_dequeue=(u32)ptr64(&event_ring[ev_i]);
        rw(rt+0x38,ev_dequeue|8);
        if(et==type){
            if(slot)*slot=sl;
            if(status)*status=(st>>24)&255;
            return 0;
        }
        /* Ignore port-change and other asynchronous events. */
    }
    return -1;
}
static int command(u32 a,u32 b,u32 c,u32 d,u32 *slot){
    u32 status=0;
    cmd_put(a,b,c,d);
    if(event_wait(TRB_COMPLETION,slot,&status,500))return -1;
    return status==COMP_SUCCESS?0:-1;
}

static void ctx32_unused(u8 *base,int idx,u32 a,u32 b,u32 c,u32 d,u32 e){
    u32 *p=(u32 *)(base+idx*32);
    p[0]=a;p[1]=b;p[2]=c;p[3]=d;p[4]=e;
}
static void ctxptr_unused(u8 *base,int idx,u64 p){
    u32 *q=(u32 *)(base+idx*32);q[2]=(u32)p;q[3]=(u32)(p>>32);
}
static int fs_interval(u8 binterval){
    int v=binterval<1?1:binterval;
    if(v>255)v=255;
    int n=3;
    while((1<<(n+1))<=v*8 && n<10)n++;
    return n;
}
static void make_link(trb_t *r){
    r[31].a=(u32)ptr64(r);r[31].b=0;r[31].c=0;
    r[31].d=TRB_TYPE(6)|TRB_LINK_TOGGLE|1;
}
static void ep_ring_reset(int index){
    for(int i=0;i<32;i++)trb_clear(&ctrl_rings[index][i]);
    make_link(ctrl_rings[index]);
}
static int xfer_wait(u32 slot,u32 *actual){
    u32 status=0;
    int r=event_wait(TRB_TRANSFER,0,&status,100);
    if(r)return -1;
    if(actual)*actual=0;
    return (status==COMP_SUCCESS||status==COMP_SHORT)?0:-1;
}

static int control_x(xdev_t *d,const u8 setup[8],void *buf,int len,int in){
    trb_t *t;
    u64 bp=ptr64(buf);
    int i=d->ep_i, cyc=d->ep_cycle;
    u32 trt=in?TRB_TRT_IN:(len?TRB_TRT_OUT:0);
    /* Setup TRB uses immediate 8-byte USB setup data, not a pointer. */
    t=&ctrl_rings[d->index][i];
    t->a=(u32)setup[0]|((u32)setup[1]<<8)|((u32)setup[2]<<16)|((u32)setup[3]<<24);
    t->b=(u32)setup[4]|((u32)setup[5]<<8)|((u32)setup[6]<<16)|((u32)setup[7]<<24);
    t->c=8;
    t->d=TRB_SETUP|TRB_IDT|TRB_CHAIN|trt|(u32)cyc;
    i++;
    if(len){
        t=&ctrl_rings[d->index][i];t->a=(u32)bp;t->b=(u32)(bp>>32);t->c=(u32)len;
        t->d=TRB_DATA|(in?TRB_DIR_IN:0)|TRB_CHAIN|(u32)cyc;i++;
    }
    t=&ctrl_rings[d->index][i];t->a=t->b=0;t->c=0;
    t->d=TRB_STATUS|(!in?TRB_DIR_IN:0)|TRB_IOC|(u32)cyc;
    i++;
    d->ep_i=i;if(i==31){d->ep_i=0;d->ep_cycle^=1;}
    rw(db+d->slot*4,1);
    return xfer_wait(d->slot,0);
}
static int getdesc(xdev_t*d,u8 type,void*buf,int len){
    u8 s[8]={0x80,6,type,0,0,0,(u8)len,(u8)(len>>8)};
    return control_x(d,s,buf,len,1);
}
static int setcfg(xdev_t*d,u8 cfg){
    u8 s[8]={0,9,cfg,0,0,0,0,0};
    return control_x(d,s,0,0,0);
}
static int setproto(xdev_t*d,u8 iface){
    u8 s[8]={0x21,11,0,0,iface,0,0,0};
    return control_x(d,s,0,0,0);
}
static int enable_slot(int *slot){
    u32 s=0;
    if(command(0,0,0,TRB_ENABLE_SLOT, &s))return -1;
    if(!s)return -1;*slot=(int)s;return 0;
}
static int address_device(xdev_t*d){
    u32 *ic=(u32 *)in_ctx;
    zero(in_ctx,2048);
    ic[1]=(1u<<0)|(1u<<1);
    u32 *sc=ctx(in_ctx,1), *ep=ctx(in_ctx,2);
    sc[0]=((u32)d->speed<<20)|(1u<<27);
    sc[1]=(u32)d->port<<16;
    ep[0]=(3u<<16); /* interval 0, CErr=3 */
    ep[1]=(4u<<3)|((u32)d->mps<<16);
    ep[2]=(u32)ptr64(ctrl_rings[d->index])|1;
    ep[3]=(u32)(ptr64(ctrl_rings[d->index])>>32);
    ep[4]=8;
    dcbaa[d->slot]=(u64)ptr64(out_ctx[d->index]);
    u32 st=0;
    if(command((u32)ptr64(in_ctx),(u32)(ptr64(in_ctx)>>32),0,
               TRB_ADDR_DEV|((u32)d->slot<<24),&st))return -1;
    return 0;
}
static int configure_ep(xdev_t*d,u8 epnum,u8 mps,u8 interval){
    zero(in_ctx,2048);
    u32 *ic=(u32 *)in_ctx;
    ic[1]=(1u<<0)|(1u<<1)|(1u<<epnum);
    u32 *sc=ctx(in_ctx,1);
    sc[0]=((u32)d->speed<<20)|(1u<<27);
    sc[1]=(u32)d->port<<16;
    u32 *e0=ctx(in_ctx,2);
    e0[0]=3u<<16;e0[1]=(4u<<3)|((u32)d->mps<<16);
    e0[2]=(u32)ptr64(ctrl_rings[d->index])|1;e0[3]=(u32)(ptr64(ctrl_rings[d->index])>>32);e0[4]=8;
    u32 *ep=ctx(in_ctx,epnum);
    trb_t *ring=(epnum&1)?kbd_rings[d->index]:mouse_rings[d->index];
    ep[0]=(3u<<1)|((u32)fs_interval(interval)<<16);
    ep[1]=(((epnum&1)?7u:3u)<<3)|((u32)mps<<16);
    ep[2]=(u32)ptr64(ring)|1;ep[3]=(u32)(ptr64(ring)>>32);
    ep[4]=8;
    return command((u32)ptr64(in_ctx),(u32)(ptr64(in_ctx)>>32),0,
                   TRB_CONFIG_EP|((u32)d->slot<<24),0);
}
static int port_reset(int p,int*speed){
    volatile u32 *ps=(volatile u32 *)(mmio+op+XOP_PORTS+p*0x10);
    u32 v=*ps;
    if(!(v&PORT_CCS))return -1;
    if(!(v&PORT_PP))*ps=v|PORT_PP;
    v=*ps;*ps=v|PORT_PR;
    for(int i=0;i<100;i++){sleep_ms(1);v=*ps;if(v&PORT_PRC)break;}
    if(!(v&PORT_PRC))return -1;
    *ps=v|PORT_PRC;
    if(speed)*speed=PORT_SPEED(v);
    return 0;
}
static int find_xhci(pci_dev_t*out){
    /* Renoir/Cezanne/Barcelo USB 3.1 controllers use 1022:1639.
     * Keep the class-code fallback so other xHCI controllers can work too. */
    if (pci_find(0x1022,0x1639,out)==0)
        return 0;
    return pci_find_class(0x0c0330,out);
}
int xhci_init(void){
    pci_dev_t d;u32 bar,cap,hcc,slots,ports;
    if(ready)return 0;
    ndev=0;
    if(find_xhci(&d))return -1;
    bar=d.bars[0];
    if(!(bar&1)&&((bar&6)==4) && d.bars[1]) return -1; /* 64-bit BAR above 32-bit address space */
    bar=pci_bar_addr(&d,0);if(!bar)return -1;
    pci_set_cmd(&d,0x06);
    mmio=(volatile u8 *)(u32)bar;
    cap=mmio[0];op=cap;
    hcc=*(volatile u32 *)(mmio+XCAP_HCC1);
    ctx_size=(hcc&(1u<<2))?64:32;
    slots=*(volatile u32 *)(mmio+XCAP_HCSP1)&0xff;
    ports=(*(volatile u32 *)(mmio+XCAP_HCSP1)>>24)&0xff;
    hcs2=*(volatile u32 *)(mmio+0x08);
    sp_count=(((hcs2>>21)&31)<<5)|((hcs2>>27)&31);
    maxslots=slots>8?8:(int)slots;maxports=ports>16?16:(int)ports;
    if(!maxslots||!maxports||sp_count>8)return -1;
    if(sp_count){
        sp_table=(u64*)kmalloc_aligned((u32)sp_count*8,64);
        if(!sp_table)return -1;
        zero(sp_table,(u32)sp_count*8);
        for(int i=0;i<sp_count;i++){
            sp_pages[i]=kmalloc_aligned(4096,4096);
            if(!sp_pages[i])return -1;
            zero(sp_pages[i],4096);
            sp_table[i]=ptr64(sp_pages[i]);
        }
    }
    db=op+*(volatile u32 *)(mmio+XCAP_DBOFF);
    rt=op+*(volatile u32 *)(mmio+XCAP_RTSOFF);
    if((*(volatile u32 *)(mmio+XOP_PAGESZ)&1)==0)return -1;
    /* Hand ownership to the OS when the legacy BIOS capability exists. */
    u32 ext=(hcc>>16)*4;
    while(ext){
        u32 v=*(volatile u32 *)(mmio+ext);
        u8 id=v&0xff;
        u8 next=(v>>8)&0xff;
        if(id==1){
            v|=(1u<<24);
            *(volatile u32 *)(mmio+ext)=v;
            for(int i=0;i<100000;i++){
                v=*(volatile u32 *)(mmio+ext);
                if(!(v&(1u<<16)))break;
            }
            break;
        }
        if(!next)break;ext+=next*4;
    }
    rw(op+XOP_CMD,rr(op+XOP_CMD)&~CMD_RS);
    if(wait32(op+XOP_STS,STS_HCH,STS_HCH,100000))return -1;
    rw(op+XOP_CMD,rr(op+XOP_CMD)|CMD_HCRST);
    if(wait32(op+XOP_CMD,CMD_HCRST,0,100000))return -1;
    if(wait32(op+XOP_STS,STS_CNR,0,100000))return -1;
    ring_init();zero(dcbaa,sizeof(dcbaa));zero(out_ctx,sizeof(out_ctx));
    if(sp_count)dcbaa[0]=ptr64(sp_table);
    rw(op+XOP_DCBAAP,(u32)ptr64(dcbaa));
    rw(op+XOP_CRCR,(u32)ptr64(cmd_ring)|1);
    rw(op+XOP_CONFIG,(u32)maxslots);
    erst[0]=(u64)ptr64(event_ring);erst[1]=64;
    rw(rt+0x28,1);
    rw(rt+0x30,(u32)ptr64(erst));
    rw(rt+0x20,0); /* interrupter disabled; we poll the event ring */
    rw(op+XOP_CMD,rr(op+XOP_CMD)|CMD_RS);
    if(wait32(op+XOP_STS,STS_HCH,0,100000))return -1;
    ready=1;
    return 0;
}
int xhci_present(void){return ready;}
int xhci_nports(void){return ready?maxports:0;}
int xhci_ndev(void){return ndev;}
int xhci_connected(int p){
    if(!ready||p<0||p>=maxports)return 0;
    return (rr(op+XOP_PORTS+p*0x10)&PORT_CCS)?1:0;
}
int xhci_enumerate_port(int p,int index){
    u8 d[18],cfg[256];int speed,slot;int total,pos,config=0,ifnum=-1;
    u8 kep=0,mep=0,kmps=8,mmps=4;
    if(index<0||index>=2||!port_reset(p,&speed))return -1;
    /* Only USB 1.x/2.0 device speeds for this first xHCI HID backend. */
    if(speed==0||speed>3)return -1;
    ep_ring_reset(index);
    for(int z=0;z<32;z++){trb_clear(&kbd_rings[index][z]);trb_clear(&mouse_rings[index][z]);}
    make_link(kbd_rings[index]);make_link(mouse_rings[index]);
    if(enable_slot(&slot))return -1;
    xdev_t *x=&devs[index];zero(x,sizeof(*x));x->used=1;x->index=index;x->port=p+1;x->slot=slot;
    x->speed=speed;x->mps=(speed==3)?64:8;x->ep_i=0;x->ep_cycle=1;
    if(address_device(x))return -1;
    if(getdesc(x,1,d,18))return -1;
    x->mps=d[7]?d[7]:x->mps;
    /* Re-addressing isn't necessary; update EP0 MPS through Evaluate Context. */
    zero(in_ctx,2048);u32 *ic=(u32*)in_ctx;ic[1]=1u<<1;
    u32 *e0=ctx(in_ctx,2);e0[1]=(4u<<3)|((u32)x->mps<<16);
    if(command((u32)ptr64(in_ctx),(u32)(ptr64(in_ctx)>>32),0,
               TRB_EVAL_CONTEXT|((u32)slot<<24),0))return -1;
    if(getdesc(x,2,cfg,9))return -1;
    total=cfg[2]|((int)cfg[3]<<8);if(total<9) return -1;if(total>256)total=256;
    if(getdesc(x,2,cfg,total))return -1;
    pos=0;
    while(pos+2<=total){
        int l=cfg[pos],t=cfg[pos+1];if(l<2||pos+l>total)break;
        if(t==2&&l>=9)config=cfg[pos+5];
        if(t==4&&l>=9&&cfg[pos+5]==3&&cfg[pos+6]==1){
            ifnum=cfg[pos+2];int proto=cfg[pos+7],eps=cfg[pos+4],q=pos+l;
            for(int e=0;e<eps&&q+2<=total;e++){int el=cfg[q],et=cfg[q+1];if(el<2||q+el>total)break;
                if(et==5&&el>=7&&(cfg[q+2]&0x80)&&((cfg[q+3]&3)==3)){
                    u8 ep=(u8)(cfg[q+2]&15);u8 m=(u8)(cfg[q+4]|((cfg[q+5]&3)<<8));
                    if(proto==1&&!kep){kep=ep;kmps=m?m:8;}
                    if(proto==2&&!mep){mep=ep;mmps=m?m:4;}
                } q+=el;
            }
        } pos+=l;
    }
    if(!config||ifnum<0||(!kep&&!mep))return -1;
    if(setcfg(x,(u8)config))return -1;
    if(setproto(x,(u8)ifnum))return -1;
    if(kep){if(configure_ep(x,(u8)(kep*2+1),kmps,10))return -1;x->kbd_ep=kep;x->kbd_mps=kmps;}
    if(mep){if(configure_ep(x,(u8)(mep*2+1),mmps,10))return -1;x->mouse_ep=mep;x->mouse_mps=mmps;}
    if(index>=ndev)ndev=index+1;
    return 0;
}
static int poll_transfer_event(u32 slot,u8 epid,u32 *status){
    trb_t *e=&event_ring[ev_i];
    if((e->d&1u)!=((u32)ev_cycle))return 0;
    u32 et=(e->d>>10)&63;
    if(et!=TRB_TRANSFER)return 0;
    if((e->d>>24)!=slot || ((e->d>>16)&31)!=epid)return 0;
    u32 st=e->c;
    ev_i++;if(ev_i==64){ev_i=0;ev_cycle^=1;}
    ev_dequeue=(u32)ptr64(&event_ring[ev_i]);rw(rt+0x38,ev_dequeue|8);
    if(status)*status=(st>>24)&255;return 1;
}
static void queue_intr(xdev_t*x,u8 ep,void*buf,int len){
    trb_t*ring=(ep&1)?kbd_rings[x->index]:mouse_rings[x->index];
    int *pi=(ep&1)?&x->k_i:&x->m_i,*pc=(ep&1)?&x->k_cycle:&x->m_cycle;
    trb_t*t=&ring[*pi];int cyc=*pc;t->a=(u32)ptr64(buf);t->b=(u32)(ptr64(buf)>>32);t->c=len;t->d=TRB_NORMAL|TRB_IOC|(u32)cyc;
    (*pi)++;if(*pi==31){ring[31].d=TRB_TYPE(6)|TRB_LINK_TOGGLE|(u32)(*pc);*pi=0;*pc^=1;}rw(db+x->slot*4,(u32)(ep*2+1));
}
int xhci_hid_trykey(int index,int *out){
    if(!ready||index<0||index>=ndev||!devs[index].used||!devs[index].kbd_ep)return -1;
    xdev_t*x=&devs[index];u32 st;
    if(!k_pending[index]){queue_intr(x,x->kbd_ep,kbuf[index],8);k_pending[index]=1;return -1;}
    int ev=poll_transfer_event(x->slot,(u8)(x->kbd_ep*2+1),&st);if(!ev)return -1;k_pending[index]=0;
    if(st!=COMP_SUCCESS&&st!=COMP_SHORT)return -1;
    u8*r=kbuf[index];x->mod=r[0];
    for(int i=0;i<6;i++){u8 k=r[2+i];int held=0;for(int j=0;j<6;j++)if(x->prev[j]==k&&k)held=1;if(!k||held)continue;x->prev[i]=k;
        if(k==0x4f){*out=0x103;return 0;}if(k==0x50){*out=0x102;return 0;}if(k==0x51){*out=0x101;return 0;}if(k==0x52){*out=0x100;return 0;}
        if(k==0x4a){*out=0x104;return 0;}if(k==0x4d){*out=0x105;return 0;}if(k==0x4c){*out=0x106;return 0;}
        char ch=0;if(k>=4&&k<=29){static const char*lo="abcdefghijklmnopqrstuvwxyz";static const char*hi="ABCDEFGHIJKLMNOPQRSTUVWXYZ";ch=(x->mod&3)?hi[k-4]:lo[k-4];}
        else if(k>=0x1e&&k<=0x27){static const char*n="1234567890";static const char*q="!@#$%^&*()";ch=(x->mod&3)?q[k-0x1e]:n[k-0x1e];}
        else switch(k){case 0x28:ch='\n';break;case 0x2c:ch=' ';break;case 0x2a:ch='\b';break;case 0x2b:ch='\t';break;case 0x2d:ch=(x->mod&3)?'_':'-';break;case 0x2e:ch=(x->mod&3)?'+':'=';break;case 0x2f:ch=(x->mod&3)?'{':'[';break;case 0x30:ch=(x->mod&3)?'}':']';break;default:break;}
        if(ch){*out=(u8)ch;return 0;}}
    for(int i=0;i<6;i++)if(!r[2+i])x->prev[i]=0;return -1;
}
int xhci_hid_mouse(int index,int*dx,int*dy,int*btn){
    if(!ready||index<0||index>=ndev||!devs[index].used||!devs[index].mouse_ep)return 0;
    xdev_t*x=&devs[index];u32 st;if(!m_pending[index]){queue_intr(x,x->mouse_ep,mbuf[index],4);m_pending[index]=1;return 0;}
    int ev=poll_transfer_event(x->slot,(u8)(x->mouse_ep*2+1),&st);if(!ev)return 0;m_pending[index]=0;if(st!=COMP_SUCCESS&&st!=COMP_SHORT)return 0;
    u8*r=mbuf[index];*btn=r[0]&7;*dx=(int)(signed char)r[1];*dy=-(int)(signed char)r[2];return 1;
}
