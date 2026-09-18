/*
 * TI TSC2007 resistive touch screen controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INPUT_TSC2007_H
#define HW_INPUT_TSC2007_H

#include "hw/i2c/i2c.h"
#include "ui/input.h"
#include "qom/object.h"

#define TYPE_TSC2007 "tsc2007"
OBJECT_DECLARE_SIMPLE_TYPE(TSC2007State, TSC2007)

struct TSC2007State {
    I2CSlave parent_obj;

    QemuInputHandlerState *input;
    qemu_irq penirq;

    /* raw adc readings at the panel corners, the guest calibrates against these */
    uint16_t raw_tl[2], raw_tr[2], raw_bl[2], raw_br[2];

    uint32_t abs[2];
    bool down;
    bool pressed;
    uint16_t sample;
    uint8_t recv_pos;
};

#endif
