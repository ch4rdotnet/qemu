/*
 * TI TVP5150AM1 video decoder, control interface only
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TVP5150_H
#define HW_MISC_TVP5150_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_TVP5150 "tvp5150"
OBJECT_DECLARE_SIMPLE_TYPE(TVP5150State, TVP5150)

#define TVP5150_NUM_REGS 256

struct TVP5150State {
    I2CSlave parent_obj;

    uint8_t regs[TVP5150_NUM_REGS];
    uint8_t pointer;
    bool pointer_next;
};

#endif
