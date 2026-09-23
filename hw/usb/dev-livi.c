/*
 * Composite USB device: a mass storage function (the homebrew stick, mounted by devb-umass
 * as /fs/usb0) and a vendor specific bulk interface that carries the livi mpeg-2 stream and
 * touch events. The unit side is sdk/out/liviplay --usb; the host side is livi/stream.py
 * talking to the chardev socket. See docs/livi-usb.md.
 *
 * This is the emulator's stand-in for the real car's USB gadget (livi/usbgadget.py): one
 * device, device class 0x00 so the qnx stack reports each interface separately, mass storage
 * on interface 1 (bulk endpoints 1/2, which the qemu msd core expects) and the vendor
 * interface on interface 0 (bulk endpoints 3/4).
 *
 * This code is licensed under the LGPL.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/usb.h"
#include "hw/usb/msd.h"
#include "hw/usb/desc.h"
#include "system/system.h"
#include "system/block-backend.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

/* host to guest buffering: the qnx hcd retries naks, so this only has to cover a burst */
#define LIVI_BUF 65536

#define TYPE_USB_LIVI "usb-livi"
OBJECT_DECLARE_SIMPLE_TYPE(USBLiviState, USB_LIVI)

struct USBLiviState {
    /* must be first: the usb core hands the class a USBDevice * that is this same address,
     * and the msd helpers upcast the scsi bus parent straight back to MSDState */
    MSDState msd;

    USBEndpoint *in_ep;
    CharFrontend cs;
    uint8_t buf[LIVI_BUF];
    /* 32 bit: a full buffer is exactly LIVI_BUF, and a 16 bit used would wrap to 0 there */
    uint32_t ptr, used;
};

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "ICC2",
    [STR_PRODUCT]      = "ICC2 livi link",
    [STR_SERIALNUMBER] = "1",
};

static const USBDescIface desc_iface_vendor_full = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = 0xff,
    .bInterfaceSubClass            = 0xff,
    .bInterfaceProtocol            = 0xff,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 64,
        },{
            .bEndpointAddress      = USB_DIR_OUT | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 64,
        },
    }
};

static const USBDescIface desc_iface_mass_full = {
    .bInterfaceNumber              = 1,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = USB_CLASS_MASS_STORAGE,
    .bInterfaceSubClass            = 0x06, /* SCSI */
    .bInterfaceProtocol            = 0x50, /* Bulk */
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 64,
        },{
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 64,
        },
    }
};

static const USBDescDevice desc_device_full = {
    .bcdUSB                        = 0x0200,
    .bDeviceClass                  = 0x00, /* per interface */
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 2,
            .bConfigurationValue   = 1,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower             = 250,
            .nif = 2,
            .ifs = (USBDescIface[]) {
                desc_iface_vendor_full,
                desc_iface_mass_full,
            },
        },
    },
};

static const USBDescIface desc_iface_vendor_high = {
    .bInterfaceNumber              = 0,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = 0xff,
    .bInterfaceSubClass            = 0xff,
    .bInterfaceProtocol            = 0xff,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x03,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 512,
        },{
            .bEndpointAddress      = USB_DIR_OUT | 0x04,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 512,
        },
    }
};

static const USBDescIface desc_iface_mass_high = {
    .bInterfaceNumber              = 1,
    .bNumEndpoints                 = 2,
    .bInterfaceClass               = USB_CLASS_MASS_STORAGE,
    .bInterfaceSubClass            = 0x06, /* SCSI */
    .bInterfaceProtocol            = 0x50, /* Bulk */
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress      = USB_DIR_IN | 0x01,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 512,
        },{
            .bEndpointAddress      = USB_DIR_OUT | 0x02,
            .bmAttributes          = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize        = 512,
        },
    }
};

static const USBDescDevice desc_device_high = {
    .bcdUSB                        = 0x0200,
    .bDeviceClass                  = 0x00, /* per interface */
    .bMaxPacketSize0               = 64,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 2,
            .bConfigurationValue   = 1,
            .bmAttributes          = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower             = 250,
            .nif = 2,
            .ifs = (USBDescIface[]) {
                desc_iface_vendor_high,
                desc_iface_mass_high,
            },
        },
    },
};

static const USBDesc desc_livi = {
    .id = {
        /* the same ids livi/usbgadget.py gives the real gadget */
        .idVendor          = 0x1209,
        .idProduct         = 0x1cc2,
        .bcdDevice         = 0x0100,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device_full,
    .high = &desc_device_high,
    .str  = desc_strings,
};

/* ---- the vendor interface ---------------------------------------------------- */

static void usb_livi_reset(USBLiviState *s)
{
    s->ptr = 0;
    s->used = 0;
}

static int usb_livi_can_read(void *opaque)
{
    USBLiviState *s = opaque;

    if (!s->msd.dev.attached) {
        return 0;
    }
    return LIVI_BUF - s->used;
}

static void usb_livi_read(void *opaque, const uint8_t *buf, int size)
{
    USBLiviState *s = opaque;
    int start, first;

    if (size > LIVI_BUF - s->used) {
        size = LIVI_BUF - s->used;
    }
    start = s->ptr + s->used;
    if (start >= LIVI_BUF) {
        start -= LIVI_BUF;
    }
    first = MIN(size, LIVI_BUF - start);
    memcpy(s->buf + start, buf, first);
    if (size > first) {
        memcpy(s->buf, buf + first, size - first);
    }
    s->used += size;
    if (s->in_ep) {
        usb_wakeup(s->in_ep, 0);
    }
}

static void usb_livi_event(void *opaque, QEMUChrEvent event)
{
    /* the mass storage side is always present, so a stream client coming and going (or
     * never connecting at all) must not change whether the unit sees the device */
}

static void usb_livi_token_out(USBLiviState *s, USBPacket *p)
{
    int i;

    for (i = 0; i < p->iov.niov; i++) {
        struct iovec *iov = p->iov.iov + i;

        qemu_chr_fe_write_all(&s->cs, iov->iov_base, iov->iov_len);
    }
    p->actual_length = p->iov.size;
}

static void usb_livi_token_in(USBLiviState *s, USBPacket *p)
{
    int len, first;

    if (!s->used) {
        p->status = USB_RET_NAK;
        return;
    }
    len = MIN(p->iov.size, s->used);
    first = MIN(len, LIVI_BUF - s->ptr);
    usb_packet_copy(p, s->buf + s->ptr, first);
    if (len > first) {
        usb_packet_copy(p, s->buf, len - first);
    }
    s->used -= len;
    s->ptr = (s->ptr + len) % LIVI_BUF;
}

static void usb_livi_handle_data(USBDevice *dev, USBPacket *p)
{
    USBLiviState *s = USB_LIVI(dev);
    uint8_t ep = p->ep->nr;

    if (p->pid == USB_TOKEN_OUT) {
        if (ep == 2) {
            usb_msd_handle_data(dev, p);
        } else if (ep == 4) {
            usb_livi_token_out(s, p);
        } else {
            p->status = USB_RET_STALL;
        }
    } else if (p->pid == USB_TOKEN_IN) {
        if (ep == 1) {
            usb_msd_handle_data(dev, p);
        } else if (ep == 3) {
            usb_livi_token_in(s, p);
        } else {
            p->status = USB_RET_STALL;
        }
    } else {
        p->status = USB_RET_STALL;
    }
}

static void usb_livi_cancel_packet(USBDevice *dev, USBPacket *p)
{
    /* an msd packet parked waiting on scsi completion */
    MSDState *s = (MSDState *)dev;

    if (s->packet == p) {
        s->packet = NULL;
        if (s->req) {
            scsi_req_cancel(s->req);
        }
    }
}

/* ---- the device -------------------------------------------------------------- */

static const struct SCSIBusInfo usb_livi_scsi_info = {
    .tcq = false,
    .max_target = 0,
    .max_lun = 0,

    .transfer_data = usb_msd_transfer_data,
    .complete = usb_msd_command_complete,
    .cancel = usb_msd_request_cancelled,
    .load_request = usb_msd_load_request,
};

static void usb_livi_handle_reset(USBDevice *dev)
{
    USBLiviState *s = USB_LIVI(dev);

    usb_msd_handle_reset(dev);
    usb_livi_reset(s);
}

static void usb_livi_realize(USBDevice *dev, Error **errp)
{
    USBLiviState *s = USB_LIVI(dev);
    MSDState *m = &s->msd;
    BlockBackend *blk = m->conf.blk;
    SCSIDevice *scsi_dev;

    if (!blk) {
        error_setg(errp, "drive property not set");
        return;
    }
    if (!qemu_chr_fe_backend_connected(&s->cs)) {
        error_setg(errp, "chardev property is required");
        return;
    }

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    dev->flags |= (1 << USB_DEV_FLAG_IS_SCSI_STORAGE);
    scsi_bus_init(&m->bus, sizeof(m->bus), DEVICE(dev), &usb_livi_scsi_info);

    /* the same detach/re-add dance dev-storage-classic does, so the scsi bus can own the
     * block backend instead of the usb device */
    blk_ref(blk);
    blk_detach_dev(blk, DEVICE(dev));
    m->conf.blk = NULL;
    scsi_dev = scsi_bus_legacy_add_drive(&m->bus, blk, 0, !!m->removable,
                                         &m->conf, dev->serial, errp);
    blk_unref(blk);
    if (!scsi_dev) {
        return;
    }
    m->scsi_dev = scsi_dev;

    usb_msd_handle_reset(dev);
    usb_livi_reset(s);
    qemu_chr_fe_set_handlers(&s->cs, usb_livi_can_read, usb_livi_read,
                             usb_livi_event, NULL, s, NULL, true);
    s->in_ep = usb_ep_get(dev, USB_TOKEN_IN, 3);
}

static const Property usb_livi_properties[] = {
    DEFINE_BLOCK_PROPERTIES(MSDState, conf),
    DEFINE_BLOCK_ERROR_PROPERTIES(MSDState, conf),
    DEFINE_PROP_BOOL("removable", MSDState, removable, false),
    DEFINE_PROP_CHR("chardev", USBLiviState, cs),
};

static void usb_livi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "ICC2 livi link";
    uc->usb_desc       = &desc_livi;
    uc->realize        = usb_livi_realize;
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = usb_livi_handle_reset;
    uc->handle_control = usb_msd_handle_control;
    uc->handle_data    = usb_livi_handle_data;
    uc->cancel_packet  = usb_livi_cancel_packet;
    device_class_set_props(dc, usb_livi_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo usb_livi_info = {
    .name          = TYPE_USB_LIVI,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBLiviState),
    .class_init    = usb_livi_class_init,
};

static void usb_livi_register_types(void)
{
    type_register_static(&usb_livi_info);
}

type_init(usb_livi_register_types)
