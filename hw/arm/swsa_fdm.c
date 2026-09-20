/*
 * swsa fdm, the i.MX31 board inside the ford icc head unit
 *
 * 512mb over both sdram chip selects, serial 0 is the debug console on
 * uart1, serial 2 is uart3 which goes to the can companion micro. uart4 and
 * uart5 are modelled too, serial 4 is uart5, the spare gps port (/dev/ser5).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/fsl-imx31.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/input/tsc2007.h"
#include "hw/misc/tvp5150.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/qtest.h"
#include "system/reset.h"

typedef struct SWSAFDM {
    FslIMX31State soc;
} SWSAFDM;

/* the board marks itself xtal_20MHz, startup derives every clock from that */
#define CKIH_FREQUENCY      20000000

/*
 * we enter at qnx startup, so the ccm has to look like the ipl already ran,
 * mcu pll at 532mhz (mfi 13, mfn 3, mfd 9), ahb /4, ipg /2, per /8
 */
#define IPL_MPCTL           0x00093403
#define IPL_PDR0            0xff870b58

/* reversing camera video decoder */
#define CAMERA_I2C_BUS     0   /* i2c1 */
#define CAMERA_I2C_ADDR    0x5d

#define TOUCH_I2C_BUS      1   /* i2c2 */
#define TOUCH_I2C_ADDR     0x48
#define TOUCH_PENIRQ_PORT  1   /* gpio2 */
#define TOUCH_PENIRQ_PIN   13

/* raw tsc2007 readings at the panel corners, from a unit's stored calibration */
static const struct {
    const char *prop;
    uint16_t raw;
} touch_corners[] = {
    { "x-top-left", 3942 },    { "y-top-left", 3851 },
    { "x-top-right", 126 },    { "y-top-right", 3841 },
    { "x-bottom-left", 3897 }, { "y-bottom-left", 137 },
    { "x-bottom-right", 81 },  { "y-bottom-right", 127 },
};

static void swsa_fdm_touch_init(FslIMX31State *soc)
{
    DeviceState *dev = DEVICE(i2c_slave_new(TYPE_TSC2007, TOUCH_I2C_ADDR));
    DeviceState *gpio = DEVICE(&soc->gpio[TOUCH_PENIRQ_PORT]);

    for (int i = 0; i < ARRAY_SIZE(touch_corners); i++) {
        qdev_prop_set_uint16(dev, touch_corners[i].prop, touch_corners[i].raw);
    }
    i2c_slave_realize_and_unref(I2C_SLAVE(dev), soc->i2c[TOUCH_I2C_BUS].bus,
                                &error_fatal);
    qdev_connect_gpio_out_named(dev, "penirq", 0,
                                qdev_get_gpio_in(gpio, TOUCH_PENIRQ_PIN));
}

static SWSAFDM *fdm;

/* has to land after the devices reset, a plain reset handler runs too early */
static void swsa_fdm_reset(MachineState *machine, ResetType type)
{
    SWSAFDM *s = fdm;

    qemu_devices_reset(type);
    s->soc.ccm.reg[IMX31_CCM_MPCTL_REG] = IPL_MPCTL;
    s->soc.ccm.reg[IMX31_CCM_PDR0_REG] = IPL_PDR0;
}

static struct arm_boot_info swsa_fdm_binfo = {
    .loader_start = FSL_IMX31_SDRAM0_ADDR,
};

static void swsa_fdm_init(MachineState *machine)
{
    SWSAFDM *s = g_new0(SWSAFDM, 1);

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_FSL_IMX31);
    qdev_prop_set_uint32(DEVICE(&s->soc.ccm), "ckih-frequency", CKIH_FREQUENCY);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* csd0 and csd1 are contiguous, so one region covers both */
    memory_region_add_subregion(get_system_memory(), FSL_IMX31_SDRAM0_ADDR,
                                machine->ram);

    swsa_fdm_touch_init(&s->soc);
    i2c_slave_create_simple(s->soc.i2c[CAMERA_I2C_BUS].bus, TYPE_TVP5150,
                            CAMERA_I2C_ADDR);
    fdm = s;

    swsa_fdm_binfo.ram_size = machine->ram_size;
    if (!qtest_enabled()) {
        arm_load_kernel(&s->soc.cpu, machine, &swsa_fdm_binfo);
    }
}

static void swsa_fdm_machine_init(MachineClass *mc)
{
    mc->desc = "SWSA FDM, Ford ICC head unit (i.MX31)";
    mc->init = swsa_fdm_init;
    mc->reset = swsa_fdm_reset;
    /* the guest pokes nand, usb and iomux blocks that aren't modelled */
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_size = 512 * MiB;
    mc->default_ram_id = "swsa-fdm.ram";
}

DEFINE_MACHINE_ARM("swsa-fdm", swsa_fdm_machine_init)
