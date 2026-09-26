#ifndef XHCI_H
#define XHCI_H
int xhci_init(void);
int xhci_present(void);
int xhci_nports(void);
int xhci_ndev(void);
int xhci_connected(int port);
int xhci_enumerate_port(int port,int index);
int xhci_hid_trykey(int index,int *out);
int xhci_hid_mouse(int index,int *dx,int *dy,int *btn);
#endif
