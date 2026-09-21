/*
 * canvas display for the emscripten build
 *
 * the page reads a rgba copy of console 0 straight out of the shared wasm
 * memory, so nothing here touches the dom. touch goes the other way through a
 * small ring the page fills, drained on the qemu thread under the bql.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/notify.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include <emscripten.h>

#define TOUCH_RING 64

/* laid out for the page, it reads these as plain u32s at wasm_ui_info() */
typedef struct WasmUiInfo {
    uint32_t generation;
    uint32_t width;
    uint32_t height;
    uint32_t pad;
    uint64_t pixels;
    uint32_t touch_head;
    uint32_t touch_tail;
    /* x, y, down per slot */
    int32_t touch[TOUCH_RING][3];
} WasmUiInfo;

static WasmUiInfo info;
static pixman_image_t *rgba;
static DisplaySurface *surface;
static DisplayChangeListener dcl;

EMSCRIPTEN_KEEPALIVE WasmUiInfo *wasm_ui_info(void)
{
    return &info;
}

static void wasm_ui_touch(void)
{
    uint32_t tail = qatomic_load_acquire(&info.touch_tail);

    while (tail != qatomic_load_acquire(&info.touch_head)) {
        int32_t *t = info.touch[tail % TOUCH_RING];

        qemu_input_queue_abs(dcl.con, INPUT_AXIS_X, t[0], 0, info.width);
        qemu_input_queue_abs(dcl.con, INPUT_AXIS_Y, t[1], 0, info.height);
        qemu_input_queue_btn(dcl.con, INPUT_BUTTON_LEFT, t[2]);
        qemu_input_event_sync();
        qatomic_store_release(&info.touch_tail, ++tail);
    }
}

static void wasm_ui_update(DisplayChangeListener *d, int x, int y, int w, int h)
{
    if (!rgba || !surface) {
        return;
    }
    pixman_image_composite(PIXMAN_OP_SRC, surface->image, NULL, rgba,
                           x, y, 0, 0, x, y, w, h);
    qatomic_store_release(&info.generation, info.generation + 1);
}

static void wasm_ui_switch(DisplayChangeListener *d, DisplaySurface *s)
{
    surface = s;
    if (!s) {
        return;
    }
    if (rgba) {
        pixman_image_unref(rgba);
    }
    /* a8b8g8r8 is r, g, b, a in byte order, which is what imagedata wants */
    rgba = pixman_image_create_bits(PIXMAN_a8b8g8r8, surface_width(s),
                                    surface_height(s), NULL, 0);
    info.pixels = (uintptr_t)pixman_image_get_data(rgba);
    info.width = surface_width(s);
    info.height = surface_height(s);
    wasm_ui_update(d, 0, 0, info.width, info.height);
}

static void wasm_ui_refresh(DisplayChangeListener *d)
{
    wasm_ui_touch();
    qemu_console_hw_update(d->con);
}

static const DisplayChangeListenerOps wasm_ui_ops = {
    .dpy_name = "wasm",
    .dpy_refresh = wasm_ui_refresh,
    .dpy_gfx_update = wasm_ui_update,
    .dpy_gfx_switch = wasm_ui_switch,
};

/* not a -display type, it rides along with -display none so qapi stays stock */
static void wasm_ui_start(Notifier *n, void *data)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);

    if (con) {
        qemu_console_register_listener(con, &dcl, &wasm_ui_ops);
    }
}

static Notifier wasm_ui_notifier = { .notify = wasm_ui_start };

static void __attribute__((constructor)) wasm_ui_init(void)
{
    qemu_add_machine_init_done_notifier(&wasm_ui_notifier);
}
