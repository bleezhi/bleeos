#ifndef USB_H
#define USB_H
int usb_scan(void);
int usb_ndev(void);
void usb_debug_probe(void);
int usb_hid_trykey(void);
int usb_hid_mouse_poll(int *dx,int *dy,int *btn);
#endif
