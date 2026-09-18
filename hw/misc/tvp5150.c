/*
 * TI TVP5150AM1 video decoder, control interface only
 *
 * a register file that identifies as the am1 part and always reports a
 * locked pal signal, enough for a guest driver to configure it and carry on.
 * no video comes out of it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/tvp5150.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define REG_DEVICE_ID_MSB   0x80
#define REG_DEVICE_ID_LSB   0x81
#define REG_ROM_MAJOR       0x82
#define REG_ROM_MINOR       0x83
#define REG_STATUS_1        0x88
#define REG_STATUS_5        0x8c
/* everything from the device id up to the status block is read only */
#define REG_READONLY_FIRST  0x80
#define REG_READONLY_LAST   0x8f

#define DEVICE_ID           0x5150
#define ROM_MAJOR_AM1       0x04
/* vsync, hsync and colour subcarrier locked, 50hz field rate */
#define STATUS_1_LOCKED_PAL 0x2e
/* autoswitch on, pal b/g/h/i/n detected */
#define STATUS_5_PAL        0x83

static void tvp5150_reset(DeviceState *dev)
{
    TVP5150State *s = TVP5150(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[REG_DEVICE_ID_MSB] = DEVICE_ID >> 8;
    s->regs[REG_DEVICE_ID_LSB] = DEVICE_ID & 0xff;
    s->regs[REG_ROM_MAJOR] = ROM_MAJOR_AM1;
    s->regs[REG_STATUS_1] = STATUS_1_LOCKED_PAL;
    s->regs[REG_STATUS_5] = STATUS_5_PAL;
    s->pointer = 0;
    s->pointer_next = false;
}

static int tvp5150_event(I2CSlave *i2c, enum i2c_event event)
{
    TVP5150State *s = TVP5150(i2c);

    if (event == I2C_START_SEND) {
        s->pointer_next = true;
    }
    return 0;
}

/* first byte of a write is the subaddress, the rest is data for it */
static int tvp5150_send(I2CSlave *i2c, uint8_t data)
{
    TVP5150State *s = TVP5150(i2c);

    if (s->pointer_next) {
        s->pointer = data;
        s->pointer_next = false;
    } else if (s->pointer < REG_READONLY_FIRST || s->pointer > REG_READONLY_LAST) {
        s->regs[s->pointer] = data;
    }
    return 0;
}

static uint8_t tvp5150_recv(I2CSlave *i2c)
{
    TVP5150State *s = TVP5150(i2c);

    return s->regs[s->pointer];
}

static const VMStateDescription vmstate_tvp5150 = {
    .name = TYPE_TVP5150,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, TVP5150State),
        VMSTATE_UINT8_ARRAY(regs, TVP5150State, TVP5150_NUM_REGS),
        VMSTATE_UINT8(pointer, TVP5150State),
        VMSTATE_BOOL(pointer_next, TVP5150State),
        VMSTATE_END_OF_LIST()
    }
};

static void tvp5150_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = tvp5150_event;
    k->recv = tvp5150_recv;
    k->send = tvp5150_send;
    dc->vmsd = &vmstate_tvp5150;
    device_class_set_legacy_reset(dc, tvp5150_reset);
}

static const TypeInfo tvp5150_info = {
    .name = TYPE_TVP5150,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TVP5150State),
    .class_init = tvp5150_class_init,
};

static void tvp5150_register_types(void)
{
    type_register_static(&tvp5150_info);
}

type_init(tvp5150_register_types)
