/*
 * i.MX31 image processing unit, sdc display path only
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMX31_IPU_H
#define IMX31_IPU_H

#include "hw/core/sysbus.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_IMX31_IPU "imx31.ipu"
OBJECT_DECLARE_SIMPLE_TYPE(IMX31IPUState, IMX31_IPU)

#define IMX31_IPU_MMIO_SIZE 0x4000
#define IMX31_IPU_NUM_REGS  (0x200 / 4)
/* ima address is mem(4) row(13) word(3), rows past 255 are never used */
#define IMX31_IPU_IMA_SIZE  (16 * 256 * 8)

struct IMX31IPUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuConsole *con;
    QEMUTimer *frame_timer;
    qemu_irq irq;

    uint32_t regs[IMX31_IPU_NUM_REGS];
    uint32_t ima[IMX31_IPU_IMA_SIZE];
    uint32_t width;
    uint32_t height;
    bool trace;
};

#endif
