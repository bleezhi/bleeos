/* BleeOS AMD display bring-up.
 *
 * This is intentionally conservative. BleeOS currently obtains its
 * framebuffer from UEFI GOP. This driver detects the AMD Radeon display
 * controller, enables PCI memory/bus-master access, and records its MMIO BAR.
 * Actual DCN 2.1 modesetting will be layered on top of this without
 * replacing the GOP fallback until the hardware path is ready.
 */
#include "amd_display.h"
#include "pci.h"

#define AMD_VENDOR_ID 0x1002u
#define PCI_CLASS_DISPLAY 0x030000u

static int present;
static int dcn21;
static u16 device_id;
static u32 mmio_base;
static u32 dcn_probe;
static int native_ready;

typedef struct {
    u32 hdisplay, vdisplay, pixel_clock_khz;
    u32 htotal, hsync_start, hsync_end;
    u32 vtotal, vsync_start, vsync_end;
} amd_mode_t;

static const amd_mode_t native_mode = {
    1920, 1080, 148500,
    2200, 2008, 2052,
    1125, 1084, 1089
};

static int known_dcn21(u16 did) {
    switch (did) {
        case 0x15e7: /* Barcelo / Barcelo-R */
        case 0x1636:
        case 0x1638:
        case 0x164c:
        case 0x164e:
            return 1;
        default:
            return 0;
    }
}

int amd_display_init(void) {
    pci_dev_t d;

    present = 0;
    dcn21 = 0;
    device_id = 0;
    mmio_base = 0;
    dcn_probe = 0;
    native_ready = 0;

    if (pci_scan() <= 0)
        return -1;

    for (int i = 0; i < pci_ndev(); i++) {
        const pci_dev_t *p = pci_dev(i);
        if (!p || p->vid != AMD_VENDOR_ID || p->class != PCI_CLASS_DISPLAY)
            continue;

        d = *p;
        device_id = d.did;
        mmio_base = pci_bar_addr(&d, 0);
        if (!mmio_base)
            continue;

        pci_set_cmd(&d, 0x0006);
        present = 1;
        dcn21 = known_dcn21(device_id);
        if (dcn21) {
            /* DCN is shared with other OSes, but the actual pipeline is
             * ASIC-specific. First probe only: do not write display
             * registers until a connector/link resource is identified. */
            volatile u32 *r = (volatile u32 *)(u32)mmio_base;
            dcn_probe = r[0];
            native_ready = (dcn_probe != 0xffffffffu);
        }
        return 0;
    }

    return -1;
}

int amd_display_present(void) { return present; }
int amd_display_is_dcn21(void) { return dcn21; }
u32 amd_display_mmio(void) { return mmio_base; }
u16 amd_display_device(void) { return device_id; }

/* The first native mode is deliberately fixed to a conservative 1080p60
 * timing. EDID/link detection must precede any DCN register programming;
 * until that exists, GOP remains authoritative. */
int amd_display_native_ready(void) { return native_ready && dcn21 && native_mode.hdisplay == 1920 && native_mode.vdisplay == 1080; }
int amd_display_edid_ready(void) { return 0; }
int amd_display_mode_ready(void) { return 0; }
int amd_display_hdmi_ready(void) { return 0; }
int amd_display_present_frame(void) { return -1; }
