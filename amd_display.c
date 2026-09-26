/* BleeOS AMD display bring-up.
 *
 * This is intentionally conservative.  BleeOS currently obtains its
 * framebuffer from UEFI GOP.  This driver detects the AMD Radeon display
 * controller, enables PCI memory/bus-master access, and records its MMIO BAR.
 * Actual DCN 2.1 modesetting will be layered on top of this without
 * replacing the GOP fallback until the hardware path is ready.
 */
#include "amd_display.h"
#include "pci.h"

#define AMD_VENDOR_ID 0x1002u
#define PCI_CLASS_DISPLAY 0x0300u

static int present;
static int dcn21;
static u16 device_id;
static u32 mmio_base;

static int known_dcn21(u16 did) {
    /* Ryzen 5000 / 7x30 mobile Radeon family (Cezanne/Barcelo/Barcelo-R).
     * Keep this list conservative; unknown AMD display devices are still
     * detected, but are not treated as DCN 2.1. */
    switch (did) {
        case 0x1636: /* Cezanne/Barcelo family */
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

    if (pci_scan() <= 0)
        return -1;

    for (int i = 0; i < pci_ndev(); i++) {
        const pci_dev_t *p = pci_dev(i);
        if (!p || p->vid != AMD_VENDOR_ID || p->class != PCI_CLASS_DISPLAY)
            continue;

        d = *p;
        device_id = d.did;
        mmio_base = pci_bar_addr(&d, 0);

        /* BAR0 is the normal register aperture on these Radeon devices.
         * Do not dereference it yet: later DCN code will validate the BAR
         * width and controller state before programming registers. */
        if (!mmio_base)
            continue;

        pci_set_cmd(&d, 0x0006); /* memory space + bus mastering */

        present = 1;
        dcn21 = known_dcn21(device_id);
        return 0;
    }

    return -1;
}

int amd_display_present(void) { return present; }
int amd_display_is_dcn21(void) { return dcn21; }
u32 amd_display_mmio(void) { return mmio_base; }
u16 amd_display_device(void) { return device_id; }

/* Placeholder for the future hardware page-flip path.  Returning non-zero
 * keeps the GOP framebuffer as the authoritative scanout for now. */
int amd_display_present(void) {
    return -1;
}
