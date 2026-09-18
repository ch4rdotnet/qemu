/*
 * i.MX31 image processing unit, sdc display path only
 *
 * models just enough of the ipu for a guest framebuffer driver, the
 * register file, the ima channel parameter memory, the sdc background and
 * foreground idmac channels and the end of frame interrupts.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/imx31_ipu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define IPU_CONF            0x00
#define IPU_CHA_BUF0_RDY    0x04
#define IPU_CHA_BUF1_RDY    0x08
#define IPU_CHA_DB_MODE_SEL 0x0c
#define IPU_CHA_CUR_BUF     0x10
#define IPU_TASKS_STAT      0x1c
#define IPU_IMA_ADDR        0x20
#define IPU_IMA_DATA        0x24
#define IPU_INT_CTRL_1      0x28
#define IPU_INT_STAT_1      0x3c
#define IPU_INT_STAT_2      0x40
#define IPU_INT_STAT_3      0x44
#define IPU_INT_STAT_5      0x4c
#define IDMAC_CHA_EN        0xa8
#define SDC_COM_CONF        0xb4
#define SDC_GW_CTRL         0xb8
#define SDC_FG_POS          0xbc

#define IPU_CONF_SDC_EN     (1 << 4)
#define SDC_COM_FG_EN       (1 << 4)
#define SDC_COM_GLB_A       (1 << 6)
#define SDC_COM_KEY_EN      (1 << 7)

#define CH_SDC_BG           14
#define CH_SDC_FG           15

/* int_stat_3 bits for the two sdc planes leaving the display interface */
#define STAT3_SDC_BG_OUT_EOF (1 << 1)
#define STAT3_SDC_FG_OUT_EOF (1 << 2)
#define STAT3_DISP3_VSYNC    (1 << 16)

#define FRAME_PERIOD_NS     (NANOSECONDS_PER_SECOND / 60)

#define R(s, off) ((s)->regs[(off) / 4])

typedef struct {
    hwaddr addr;
    uint32_t width, height, stride, bpp;
} IPUPlane;

static uint32_t ima_index(uint32_t addr)
{
    return (((addr >> 16) & 0xf) << 11) | (((addr >> 3) & 0xff) << 3) |
           (addr & 7);
}

static uint32_t ima_word(IMX31IPUState *s, int row, int word)
{
    return s->ima[ima_index(0x10000 | (row << 3) | word)];
}

static bool plane_get(IMX31IPUState *s, int ch, IPUPlane *p)
{
    static const uint8_t bpp_code[8] = { 32, 24, 16, 8, 4, 0, 0, 0 };
    uint32_t w3, w4, cur, fmt;

    if (!(R(s, IDMAC_CHA_EN) & (1u << ch))) {
        return false;
    }
    w3 = ima_word(s, 2 * ch, 3);
    w4 = ima_word(s, 2 * ch, 4);
    fmt = ima_word(s, 2 * ch + 1, 2);
    cur = (R(s, IPU_CHA_CUR_BUF) >> ch) & 1;

    p->width = ((w3 >> 12) & 0xfff) + 1;
    p->height = (((w3 >> 24) & 0xff) | ((w4 & 0xf) << 8)) + 1;
    p->bpp = bpp_code[fmt & 7];
    p->stride = ((fmt >> 3) & 0x3fff) + 1;
    p->addr = ima_word(s, 2 * ch + 1, cur);
    return p->addr && p->bpp >= 16;
}

static uint32_t plane_pixel(const IPUPlane *p, const uint8_t *src)
{
    uint32_t v;

    switch (p->bpp) {
    case 16:
        v = lduw_le_p(src);
        return (((v >> 11) & 0x1f) << 19) | (((v >> 5) & 0x3f) << 10) |
               ((v & 0x1f) << 3);
    case 24:
        return src[0] | (src[1] << 8) | (src[2] << 16);
    default:
        return ldl_le_p(src) & 0xffffff;
    }
}

static uint8_t *plane_read(const IPUPlane *p)
{
    size_t len = (size_t)p->stride * p->height;
    uint8_t *buf = g_malloc(len);

    address_space_read(&address_space_memory, p->addr, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
    return buf;
}

static void imx31_ipu_draw_bg(IMX31IPUState *s, const IPUPlane *p)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    g_autofree uint8_t *buf = plane_read(p);
    uint32_t *dst = surface_data(surface);
    int step = p->bpp / 8;

    for (uint32_t y = 0; y < p->height; y++) {
        const uint8_t *src = buf + (size_t)y * p->stride;
        for (uint32_t x = 0; x < p->width; x++, src += step) {
            dst[x] = plane_pixel(p, src);
        }
        dst = (uint32_t *)((uint8_t *)dst + surface_stride(surface));
    }
}

/*
 * foreground window. the graphic window is always taken to be the foreground
 * (gwsel = 1, which is how the qnx driver leaves it), global alpha and the
 * colour key come from sdc_gw_ctrl
 */
static void imx31_ipu_draw_fg(IMX31IPUState *s, const IPUPlane *p)
{
    DisplaySurface *surface = qemu_console_surface(s->con);
    g_autofree uint8_t *buf = plane_read(p);
    uint32_t gw = R(s, SDC_GW_CTRL), com = R(s, SDC_COM_CONF);
    uint32_t alpha = (com & SDC_COM_GLB_A) ? gw >> 24 : 255;
    uint32_t key = gw & 0xffffff;
    uint32_t x0 = (R(s, SDC_FG_POS) >> 16) & 0x3ff;
    uint32_t y0 = R(s, SDC_FG_POS) & 0x3ff;
    int step = p->bpp / 8;

    for (uint32_t y = 0; y < p->height && y + y0 < s->height; y++) {
        const uint8_t *src = buf + (size_t)y * p->stride;
        uint32_t *dst = (uint32_t *)((uint8_t *)surface_data(surface) +
                                     (y + y0) * surface_stride(surface));
        for (uint32_t x = 0; x < p->width && x + x0 < s->width;
             x++, src += step) {
            uint32_t fg = plane_pixel(p, src), bg = dst[x + x0], out = 0;

            if ((com & SDC_COM_KEY_EN) && fg == key) {
                continue;
            }
            for (int c = 0; c < 24; c += 8) {
                uint32_t f = (fg >> c) & 0xff, b = (bg >> c) & 0xff;
                out |= ((f * alpha + b * (255 - alpha)) / 255) << c;
            }
            dst[x + x0] = out;
        }
    }
}

static bool imx31_ipu_gfx_update(void *opaque)
{
    IMX31IPUState *s = opaque;
    IPUPlane bg, fg;

    if (!(R(s, IPU_CONF) & IPU_CONF_SDC_EN) || !plane_get(s, CH_SDC_BG, &bg)) {
        return true;
    }
    if (bg.width != s->width || bg.height != s->height) {
        s->width = bg.width;
        s->height = bg.height;
        qemu_console_resize(s->con, s->width, s->height);
    }
    imx31_ipu_draw_bg(s, &bg);
    if ((R(s, SDC_COM_CONF) & SDC_COM_FG_EN) && plane_get(s, CH_SDC_FG, &fg)) {
        imx31_ipu_draw_fg(s, &fg);
    }
    qemu_console_update(s->con, 0, 0, s->width, s->height);
    return true;
}

static void imx31_ipu_update_irq(IMX31IPUState *s)
{
    bool level = false;

    for (int i = 0; i < 5; i++) {
        level |= s->regs[IPU_INT_STAT_1 / 4 + i] & s->regs[IPU_INT_CTRL_1 / 4 + i];
    }
    qemu_set_irq(s->irq, level);
}

/* one frame scanned out, flip double buffered channels and flag end of frame */
static void imx31_ipu_channel_eof(IMX31IPUState *s, int ch, uint32_t out_eof)
{
    uint32_t bit = 1u << ch;

    if (!(R(s, IDMAC_CHA_EN) & bit)) {
        return;
    }
    if (R(s, IPU_CHA_DB_MODE_SEL) & bit) {
        bool cur = R(s, IPU_CHA_CUR_BUF) & bit;
        uint32_t next_rdy = cur ? IPU_CHA_BUF0_RDY : IPU_CHA_BUF1_RDY;

        if (R(s, next_rdy) & bit) {
            R(s, IPU_CHA_CUR_BUF) ^= bit;
            R(s, next_rdy) &= ~bit;
        }
    } else {
        R(s, IPU_CHA_BUF0_RDY) &= ~bit;
    }
    R(s, IPU_INT_STAT_1) |= bit;
    R(s, IPU_INT_STAT_2) |= bit;
    R(s, IPU_INT_STAT_3) |= out_eof;
}

static void imx31_ipu_frame(void *opaque)
{
    IMX31IPUState *s = opaque;

    if (R(s, IPU_CONF) & IPU_CONF_SDC_EN) {
        imx31_ipu_channel_eof(s, CH_SDC_BG, STAT3_SDC_BG_OUT_EOF);
        imx31_ipu_channel_eof(s, CH_SDC_FG, STAT3_SDC_FG_OUT_EOF);
        R(s, IPU_INT_STAT_3) |= STAT3_DISP3_VSYNC;
        imx31_ipu_update_irq(s);
    }
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
}

static void ima_advance(IMX31IPUState *s)
{
    uint32_t addr = R(s, IPU_IMA_ADDR) + 1;

    /* rows are five words long, the next access lands on word 0 of the next */
    if ((addr & 7) == 5) {
        addr = (addr & ~7u) + 8;
    }
    R(s, IPU_IMA_ADDR) = addr;
}

static uint64_t imx31_ipu_read(void *opaque, hwaddr offset, unsigned size)
{
    IMX31IPUState *s = opaque;
    uint32_t value = 0;

    if (offset == IPU_IMA_DATA) {
        value = s->ima[ima_index(R(s, IPU_IMA_ADDR))];
        ima_advance(s);
    } else if (offset / 4 < IMX31_IPU_NUM_REGS) {
        value = s->regs[offset / 4];
    }
    if (s->trace) {
        qemu_log("imx31.ipu: read  %03" HWADDR_PRIx " = %08x\n", offset, value);
    }
    return value;
}

static void imx31_ipu_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    IMX31IPUState *s = opaque;

    if (s->trace) {
        qemu_log("imx31.ipu: write %03" HWADDR_PRIx " = %08x\n", offset,
                 (uint32_t)value);
    }
    if (offset / 4 >= IMX31_IPU_NUM_REGS) {
        return;
    }
    switch (offset) {
    case IPU_CHA_BUF0_RDY:
    case IPU_CHA_BUF1_RDY:
        s->regs[offset / 4] |= value;
        break;
    case IPU_IMA_DATA:
        s->ima[ima_index(R(s, IPU_IMA_ADDR))] = value;
        ima_advance(s);
        break;
    case IPU_TASKS_STAT:
        /* write one to clear, nothing here ever sets a task error */
        s->regs[offset / 4] &= ~value;
        break;
    case IPU_INT_STAT_1 ... IPU_INT_STAT_5:
        s->regs[offset / 4] &= ~value;
        imx31_ipu_update_irq(s);
        break;
    case IPU_INT_CTRL_1 ... IPU_INT_CTRL_1 + 0x10:
        s->regs[offset / 4] = value;
        imx31_ipu_update_irq(s);
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps imx31_ipu_ops = {
    .read = imx31_ipu_read,
    .write = imx31_ipu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void imx31_ipu_invalidate(void *opaque)
{
}

static const GraphicHwOps imx31_ipu_gfx_ops = {
    .invalidate = imx31_ipu_invalidate,
    .gfx_update = imx31_ipu_gfx_update,
};

static void imx31_ipu_reset(DeviceState *dev)
{
    IMX31IPUState *s = IMX31_IPU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ima, 0, sizeof(s->ima));
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
}

static void imx31_ipu_realize(DeviceState *dev, Error **errp)
{
    IMX31IPUState *s = IMX31_IPU(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &imx31_ipu_ops, s,
                          TYPE_IMX31_IPU, IMX31_IPU_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, imx31_ipu_frame, s);
    s->con = qemu_graphic_console_create(dev, 0, &imx31_ipu_gfx_ops, s);
}

static const VMStateDescription vmstate_imx31_ipu = {
    .name = TYPE_IMX31_IPU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IMX31IPUState, IMX31_IPU_NUM_REGS),
        VMSTATE_UINT32_ARRAY(ima, IMX31IPUState, IMX31_IPU_IMA_SIZE),
        VMSTATE_TIMER_PTR(frame_timer, IMX31IPUState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property imx31_ipu_props[] = {
    DEFINE_PROP_BOOL("trace", IMX31IPUState, trace, false),
};

static void imx31_ipu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, imx31_ipu_props);
    dc->realize = imx31_ipu_realize;
    dc->vmsd = &vmstate_imx31_ipu;
    device_class_set_legacy_reset(dc, imx31_ipu_reset);
}

static const TypeInfo imx31_ipu_info = {
    .name = TYPE_IMX31_IPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMX31IPUState),
    .class_init = imx31_ipu_class_init,
};

static void imx31_ipu_register_types(void)
{
    type_register_static(&imx31_ipu_info);
}

type_init(imx31_ipu_register_types)
