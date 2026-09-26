/* USB core + HID boot-protocol enumeration. */
#include "usb.h"
#include "uhci.h"
#include "xhci.h"
#include "drivers.h"
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_ADDRESS 5
#define USB_REQ_SET_CONFIG 9
#define USB_REQ_SET_PROTOCOL 11
#define USB_DESC_DEVICE 1
#define USB_DESC_CONFIG 2
#define USB_CLASS_HID 3
#define USB_HID_BOOT 1
#define USB_HID_KEYBOARD 1
#define USB_HID_MOUSE 2
typedef struct {
    int used, low, caps;
    u8 addr, maxpkt, kbd_ep, mouse_ep, kbd_mps, mouse_mps;
    int kbd_toggle, mouse_toggle;
    u8 kbd_prev[6], kbd_mod;
} hid_dev_t;
static hid_dev_t devs[2];
static int ndev, scanned, xndev;
static int ctrl(u8 a,u8 m,int low,u8 rt,u8 req,u16 val,u16 idx,
                void *buf,int len,int in) {
    u8 s[8];
    s[0]=rt;s[1]=req;s[2]=(u8)val;s[3]=(u8)(val>>8);
    s[4]=(u8)idx;s[5]=(u8)(idx>>8);s[6]=(u8)len;s[7]=(u8)(len>>8);
    return uhci_control(a,m,low,s,buf,len,in);
}
static int desc(u8 a,u8 m,int low,u8 type,void *buf,int len) {
    return ctrl(a,m,low,0x80,USB_REQ_GET_DESCRIPTOR,(u16)type<<8,0,buf,len,1);
}
static int enumerate_port(int port) {
    u8 d[18],cfg[256];
    int low,mps,addr,total,pos,config=0,ifnum=-1;
    u8 kep=0,mep=0,kmps=8,mmps=4;
    hid_dev_t *hd;
    if(uhci_port_reset(port,&low))return -1;
    if(desc(0,8,low,USB_DESC_DEVICE,d,8))return -1;
    if(d[0]<8||d[1]!=USB_DESC_DEVICE)return -1;
    mps=d[7]?d[7]:8;addr=ndev+1;
    {u8 s[8]={0,USB_REQ_SET_ADDRESS,(u8)addr,0,0,0,0,0};
     if(uhci_control(0,(u8)mps,low,s,0,0,0))return -1;}
    sleep_ms(2);
    if(desc((u8)addr,(u8)mps,low,USB_DESC_DEVICE,d,18))return -1;
    if(ctrl((u8)addr,(u8)mps,low,0x80,USB_REQ_GET_DESCRIPTOR,
            USB_DESC_CONFIG<<8,0,cfg,9,1))return -1;
    total=(int)cfg[2]|((int)cfg[3]<<8);
    if(total<9)return -1;
    if(total>(int)sizeof(cfg))total=sizeof(cfg);
    if(ctrl((u8)addr,(u8)mps,low,0x80,USB_REQ_GET_DESCRIPTOR,
            USB_DESC_CONFIG<<8,0,cfg,total,1))return -1;
    pos=0;
    while(pos+2<=total) {
        int l=cfg[pos],t=cfg[pos+1];
        if(l<2||pos+l>total)break;
        if(t==2&&l>=9)config=cfg[pos+5];
        if(t==4&&l>=9&&cfg[pos+5]==USB_CLASS_HID&&cfg[pos+6]==USB_HID_BOOT) {
            int proto=cfg[pos+7],eps=cfg[pos+4],q=pos+l;
            ifnum=cfg[pos+2];
            for(int e=0;e<eps&&q+2<=total;e++) {
                int el=cfg[q],et=cfg[q+1];
                if(el<2||q+el>total)break;
                if(et==5&&el>=7&&(cfg[q+2]&0x80)&&((cfg[q+3]&3)==3)) {
                    u8 ep=cfg[q+2]&15;
                    u16 mp=(u16)cfg[q+4]|((u16)cfg[q+5]<<8);
                    if(proto==USB_HID_KEYBOARD&&!kep){kep=ep;kmps=(u8)(mp>8?8:mp);}
                    if(proto==USB_HID_MOUSE&&!mep){mep=ep;mmps=(u8)(mp>4?4:mp);}
                }
                q+=el;
            }
        }
        pos+=l;
    }
    if(ndev>=2||ifnum<0||(!kep&&!mep)||!config)return -1;
    if(ctrl((u8)addr,(u8)mps,low,0,USB_REQ_SET_CONFIG,(u16)config,0,0,0,0))return -1;
    if(kep&&ctrl((u8)addr,(u8)mps,low,0x21,USB_REQ_SET_PROTOCOL,0,(u16)ifnum,0,0,0))return -1;
    hd=&devs[ndev++];hd->used=1;hd->low=low;hd->addr=(u8)addr;hd->maxpkt=(u8)mps;
    hd->kbd_ep=kep;hd->mouse_ep=mep;hd->kbd_mps=kmps;hd->mouse_mps=mmps;
    hd->kbd_toggle=hd->mouse_toggle=0;hd->kbd_mod=0;hd->caps=0;
    for(int i=0;i<6;i++)hd->kbd_prev[i]=0;
    return 0;
}
int usb_scan(void) {
    if(scanned)return ndev;scanned=1;ndev=0;xndev=0;
    if(xhci_init()==0){
        for(int p=0;p<xhci_nports()&&xndev<2;p++)
            if(xhci_connected(p)&&xhci_enumerate_port(p,xndev)==0)xndev++;
        ndev+=xndev;
    }
    if(uhci_init()==0){
        for(int p=0;p<uhci_nports()&&ndev<2;p++)if(uhci_connected(p)&&!enumerate_port(p)) {
            char b[12];klog("usb: UHCI HID device enumerated (addr ");
            klog(utoa10(devs[ndev-1].addr,b));klog(")\n");
        }
    }
    return ndev;
}
int usb_ndev(void){return ndev;}
void usb_debug_probe(void){
    char b[12];vga_print("usb: UHCI ");
    vga_print(uhci_present()?"active":"not found");
    vga_print(", HID devices=");vga_print(utoa10((u32)ndev,b));vga_print("\n");
}
static int held(const u8 *old,u8 k){for(int i=0;i<6;i++)if(old[i]==k)return 1;return 0;}
static char keychar(u8 k,int shift,int caps) {
    char c=0;
    if(k>=0x04&&k<=0x1d) {
        static const char *lo="abcdefghijklmnopqrstuvwxyz";
        static const char *hi="ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        c=(shift?hi:lo)[k-4];
        if(caps)c=(char)(c^32);
        return c;
    }
    switch(k) {
    case 0x1e:return shift?'!':'1'; case 0x1f:return shift?'@':'2';
    case 0x20:return shift?'#':'3'; case 0x21:return shift?'$':'4';
    case 0x22:return shift?'%':'5'; case 0x23:return shift?'^':'6';
    case 0x24:return shift?'&':'7'; case 0x25:return shift?'*':'8';
    case 0x26:return shift?'(':'9'; case 0x27:return shift?')':'0';
    case 0x28:return '\n'; case 0x2a:return '\b'; case 0x2b:return '\t';
    case 0x2c:return ' ';
    case 0x2d:return shift?'_':'-'; case 0x2e:return shift?'+':'=';
    case 0x2f:return shift?'{':'['; case 0x30:return shift?'}':']';
    case 0x31:return shift?'|':'\\'; case 0x33:return shift?':':';';
    case 0x34:return shift?'"':'\'';
    case 0x35:return shift?'~':'\x60';
    case 0x36:return shift?'<':','; case 0x37:return shift?'>':'.';
    case 0x38:return shift?'?':'/';
    default:return c;
    }
}
int usb_hid_trykey(void) {
    int k;
    for(int i=0;i<xndev;i++) if(xhci_hid_trykey(i,&k)==0) return k;
    u8 r[8];
    for(int d=0;d<ndev-xndev;d++){hid_dev_t*h=&devs[d];
        if(!h->used||!h->kbd_ep)continue;
        if(uhci_intr_in(h->addr,h->kbd_ep,h->kbd_mps,h->low,r,8,&h->kbd_toggle))continue;
        h->kbd_mod=r[0];
        for(int i=0;i<6;i++){u8 k=r[2+i];if(!k||held(h->kbd_prev,k))continue;
            h->kbd_prev[i]=k;
            if(k==0x39){h->kbd_mod^=0x40;return 0;}
            if(k==0x4f)return KEY_RIGHT;if(k==0x50)return KEY_LEFT;
            if(k==0x51)return KEY_DOWN;if(k==0x52)return KEY_UP;
            if(k==0x4a)return KEY_HOME;if(k==0x4d)return KEY_END;if(k==0x4c)return KEY_DEL;
            if(k==0x1d&&(h->kbd_mod&0x22))return 3;
            if(k==0x07&&(h->kbd_mod&0x22))return 4;
            {char c=keychar(k,(h->kbd_mod&0x22)!=0,(h->kbd_mod&0x40)!=0);if(c)return (int)(u8)c;}
        }
        for(int i=0;i<6;i++)if(!r[2+i])h->kbd_prev[i]=0;
    }
    return -1;
}
int usb_hid_mouse_poll(int *dx,int *dy,int *btn) {
    for(int i=0;i<xndev;i++) if(xhci_hid_mouse(i,dx,dy,btn)) return 1;
    u8 r[4];
    for(int d=0;d<ndev-xndev;d++){hid_dev_t*h=&devs[d];
        if(!h->used||!h->mouse_ep)continue;
        if(uhci_intr_in(h->addr,h->mouse_ep,h->mouse_mps,h->low,r,4,&h->mouse_toggle))continue;
        *btn=r[0]&7;*dx=(int)(signed char)r[1];*dy=-(int)(signed char)r[2];return 1;
    }
    return 0;
}
