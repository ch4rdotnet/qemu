/*
 * chardev for the emscripten build, -chardev wasm,id=name
 *
 * each one is a pair of byte rings in shared wasm memory that the page polls,
 * found by name through wasm_chr_info(). input is fed in on a timer, a few
 * bytes a tick, since a guest uart overruns if a whole line lands at once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include <emscripten.h>

#define WASM_CHR_MAX 4
#define WASM_CHR_RING 65536
#define WASM_CHR_TICK_MS 2

typedef struct WasmRing {
    uint32_t head;
    uint32_t tail;
    uint8_t data[WASM_CHR_RING];
} WasmRing;

/* laid out for the page, see web/qemu.js */
typedef struct WasmChrSlot {
    char name[32];
    /* bytes handed to the guest per tick, the page can change it */
    uint32_t burst;
    uint32_t pad;
    WasmRing tx;
    WasmRing rx;
} WasmChrSlot;

typedef struct WasmChardev {
    Chardev parent;
    WasmChrSlot *slot;
    QEMUTimer *timer;
} WasmChardev;

#define TYPE_CHARDEV_WASM "chardev-wasm"
DECLARE_INSTANCE_CHECKER(WasmChardev, WASM_CHARDEV, TYPE_CHARDEV_WASM)

static WasmChrSlot slots[WASM_CHR_MAX];

EMSCRIPTEN_KEEPALIVE WasmChrSlot *wasm_chr_info(void)
{
    return slots;
}

EMSCRIPTEN_KEEPALIVE uint32_t wasm_chr_count(void)
{
    return WASM_CHR_MAX;
}

static int wasm_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    WasmRing *r = &WASM_CHARDEV(chr)->slot->tx;
    uint32_t head = r->head;
    int i;

    /* a full ring drops, blocking here would stall the whole emulator on a slow page */
    for (i = 0; i < len && head - qatomic_load_acquire(&r->tail) < WASM_CHR_RING; i++) {
        r->data[head++ % WASM_CHR_RING] = buf[i];
    }
    qatomic_store_release(&r->head, head);
    return len;
}

static void wasm_chr_tick(void *opaque)
{
    WasmChardev *w = opaque;
    WasmRing *r = &w->slot->rx;
    uint32_t tail = r->tail;
    uint32_t head = qatomic_load_acquire(&r->head);
    uint32_t n = MIN(head - tail, w->slot->burst);
    uint8_t buf[256];

    n = MIN(MIN(n, sizeof(buf)), qemu_chr_be_can_write(&w->parent));
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = r->data[tail++ % WASM_CHR_RING];
    }
    if (n) {
        qatomic_store_release(&r->tail, tail);
        qemu_chr_be_write(&w->parent, buf, n);
    }
    timer_mod(w->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + WASM_CHR_TICK_MS);
}

static bool wasm_chr_open(Chardev *chr, ChardevBackend *backend, Error **errp)
{
    WasmChardev *w = WASM_CHARDEV(chr);

    for (int i = 0; i < WASM_CHR_MAX; i++) {
        if (!slots[i].name[0]) {
            w->slot = &slots[i];
            w->slot->burst = 8;
            pstrcpy(w->slot->name, sizeof(w->slot->name), chr->label);
            w->timer = timer_new_ms(QEMU_CLOCK_REALTIME, wasm_chr_tick, w);
            wasm_chr_tick(w);
            return true;
        }
    }
    error_setg(errp, "only %d wasm chardevs", WASM_CHR_MAX);
    return false;
}

static void char_wasm_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = wasm_chr_open;
    cc->chr_write = wasm_chr_write;
}

static const TypeInfo char_wasm_type_info = {
    .name = TYPE_CHARDEV_WASM,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(WasmChardev),
    .class_init = char_wasm_class_init,
};

static void register_types(void)
{
    type_register_static(&char_wasm_type_info);
}

type_init(register_types);
