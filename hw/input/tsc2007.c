/*
 * TI TSC2007 resistive touch screen controller
 *
 * the host pointer drives the panel, position is turned back into raw adc
 * counts by interpolating between the four corner readings so a guest
 * calibrated against real hardware lands where the pointer is.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/input/tsc2007.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define CMD_FUNCTION(cmd)   ((cmd) >> 4)
#define FUNC_MEASURE_X      0xc
#define FUNC_MEASURE_Y      0xd
#define FUNC_MEASURE_Z1     0xe
#define FUNC_MEASURE_Z2     0xf

#define ADC_MAX             0xfff

static uint16_t tsc2007_raw(TSC2007State *s, int axis)
{
    int64_t max = INPUT_EVENT_ABS_MAX;
    int64_t x = s->abs[0], y = s->abs[1];
    int64_t top = s->raw_tl[axis] * (max - x) + s->raw_tr[axis] * x;
    int64_t bottom = s->raw_bl[axis] * (max - x) + s->raw_br[axis] * x;

    return (top * (max - y) + bottom * y) / (max * max);
}

/* an open panel floats x to ground and y to the rail */
static uint16_t tsc2007_measure(TSC2007State *s, uint8_t function)
{
    switch (function) {
    case FUNC_MEASURE_X:
    case FUNC_MEASURE_Z1:
        return s->pressed ? tsc2007_raw(s, 0) : 0;
    case FUNC_MEASURE_Y:
    case FUNC_MEASURE_Z2:
        return s->pressed ? tsc2007_raw(s, 1) : ADC_MAX;
    default:
        return 0;
    }
}

static int tsc2007_send(I2CSlave *i2c, uint8_t data)
{
    TSC2007State *s = TSC2007(i2c);

    s->sample = tsc2007_measure(s, CMD_FUNCTION(data));
    return 0;
}

/* 12 bits, left justified over two bytes */
static uint8_t tsc2007_recv(I2CSlave *i2c)
{
    TSC2007State *s = TSC2007(i2c);

    return s->recv_pos++ ? (s->sample << 4) & 0xf0 : s->sample >> 4;
}

static int tsc2007_event(I2CSlave *i2c, enum i2c_event event)
{
    TSC2007State *s = TSC2007(i2c);

    if (event == I2C_START_RECV) {
        s->recv_pos = 0;
    }
    return 0;
}

static void tsc2007_input_event(DeviceState *dev, QemuConsole *src,
                                QemuInputEvent *evt)
{
    TSC2007State *s = TSC2007(dev);

    if (evt->type == INPUT_EVENT_KIND_ABS) {
        if (evt->abs.axis == INPUT_AXIS_X || evt->abs.axis == INPUT_AXIS_Y) {
            s->abs[evt->abs.axis == INPUT_AXIS_Y] = evt->abs.value;
        }
    } else if (evt->type == INPUT_EVENT_KIND_BTN &&
               evt->btn.button == INPUT_BUTTON_LEFT) {
        s->down = evt->btn.down;
    }
}

static void tsc2007_input_sync(DeviceState *dev)
{
    TSC2007State *s = TSC2007(dev);

    s->pressed = s->down;
    /* penirq is active low and held for as long as the panel is touched */
    qemu_set_irq(s->penirq, !s->pressed);
}

static const QemuInputHandler tsc2007_input_handler = {
    .name = "TSC2007 touch screen",
    .mask = INPUT_EVENT_MASK_BTN | INPUT_EVENT_MASK_ABS,
    .event = tsc2007_input_event,
    .sync = tsc2007_input_sync,
};

static void tsc2007_reset(DeviceState *dev)
{
    TSC2007State *s = TSC2007(dev);

    s->down = s->pressed = false;
    s->sample = 0;
    s->recv_pos = 0;
    qemu_set_irq(s->penirq, 1);
}

static void tsc2007_realize(DeviceState *dev, Error **errp)
{
    TSC2007State *s = TSC2007(dev);

    qdev_init_gpio_out_named(dev, &s->penirq, "penirq", 1);
    s->input = qemu_input_handler_register(dev, &tsc2007_input_handler);
    qemu_input_handler_activate(s->input);
}

static const VMStateDescription vmstate_tsc2007 = {
    .name = TYPE_TSC2007,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, TSC2007State),
        VMSTATE_UINT32_ARRAY(abs, TSC2007State, 2),
        VMSTATE_BOOL(down, TSC2007State),
        VMSTATE_BOOL(pressed, TSC2007State),
        VMSTATE_UINT16(sample, TSC2007State),
        VMSTATE_UINT8(recv_pos, TSC2007State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tsc2007_props[] = {
    DEFINE_PROP_UINT16("x-top-left", TSC2007State, raw_tl[0], 0),
    DEFINE_PROP_UINT16("y-top-left", TSC2007State, raw_tl[1], 0),
    DEFINE_PROP_UINT16("x-top-right", TSC2007State, raw_tr[0], ADC_MAX),
    DEFINE_PROP_UINT16("y-top-right", TSC2007State, raw_tr[1], 0),
    DEFINE_PROP_UINT16("x-bottom-left", TSC2007State, raw_bl[0], 0),
    DEFINE_PROP_UINT16("y-bottom-left", TSC2007State, raw_bl[1], ADC_MAX),
    DEFINE_PROP_UINT16("x-bottom-right", TSC2007State, raw_br[0], ADC_MAX),
    DEFINE_PROP_UINT16("y-bottom-right", TSC2007State, raw_br[1], ADC_MAX),
};

static void tsc2007_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = tsc2007_event;
    k->recv = tsc2007_recv;
    k->send = tsc2007_send;
    dc->realize = tsc2007_realize;
    dc->vmsd = &vmstate_tsc2007;
    device_class_set_legacy_reset(dc, tsc2007_reset);
    device_class_set_props(dc, tsc2007_props);
}

static const TypeInfo tsc2007_info = {
    .name = TYPE_TSC2007,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(TSC2007State),
    .class_init = tsc2007_class_init,
};

static void tsc2007_register_types(void)
{
    type_register_static(&tsc2007_info);
}

type_init(tsc2007_register_types)
