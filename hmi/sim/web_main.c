/*
 * Browser build of the HMI (WebAssembly, via Emscripten).
 *
 * Runs the real app + UI code against the simulated world in sim_world.c.
 * The page's script calls web_frame() from requestAnimationFrame, copies the
 * pixels to a canvas when they changed, and forwards mouse/touch input.
 */
#include <emscripten.h>
#include <string.h>

#include "app.h"
#include "lvgl.h"
#include "plat.h"
#include "sim_world.h"
#include "ui.h"

#define W 800
#define H 480

static uint16_t fb[W * H];
static uint8_t rgba[W * H * 4];
static bool dirty;
static int32_t ptr_x, ptr_y;
static bool ptr_down;
static bool release_pending;  /* the button came up before it had been held long enough */
static double press_frame_ms; /* time of the first frame that showed the current press */
static double last_frame_ms;
static bool press_framed;

/* LVGL samples the pointer every 30 ms, so a click must last longer than
 * that or it is never seen. A real touch panel always does; a mouse click
 * made by a script, or a very fast one, may not. */
#define MIN_PRESS_MS 60

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    dirty = true;
    lv_display_flush_ready(d);
}

static void pointer_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = ptr_x;
    data->point.y = ptr_y;
    data->state = ptr_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_cb(void) { return plat_millis(); }

/* Start with two paired feeders and a schedule, so there is something to see. */
static void demo_setup(void)
{
    static const fd_sched_entry_t tank1[] = {
        { 8, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 1 },
        { 18, 30, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 2 },
    };
    static const fd_sched_entry_t tank2[] = {
        { 7, 30, FD_ENTRY_ENABLED | FD_DOW_ALL, 8, 1 },
        { 12, 30, FD_ENTRY_ENABLED | 0x3E, 8, 1 }, /* weekdays */
        { 19, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, 8, 2 },
    };
    /* These feeders have been running for a while: their clocks are set. */
    sim_node_set_clock(0, plat_local_epoch());
    sim_node_set_clock(1, plat_local_epoch());
    sim_node_prepair(0, 1);
    sim_node_prepair(1, 2);
    int a = app_adopt(fnodes[0].mac);
    int b = app_adopt(fnodes[1].mac);
    app_rename_node(a, "Goldfish");
    app_rename_node(b, "Tetras");

    app_node_cfg_t *n = app_node(a);
    memcpy(n->entries, tank1, sizeof(tank1));
    n->count = 2;
    app_schedule_changed(a);

    /* The first feeder is calibrated (0.6 g per pulse) and has a scale under
     * its hopper; the second has neither, so every way of showing a portion
     * can be seen. */
    app_set_calibration(a, 10, 6000);
    fnodes[0].scale_fitted = fnodes[0].scale_zeroed = fnodes[0].scale_ready = true;
    fnodes[0].hopper_mg = 180000;
    fnodes[0].true_mg_per_pulse = 580;

    n = app_node(b);
    memcpy(n->entries, tank2, sizeof(tank2));
    n->count = 3;
    app_schedule_changed(b);
}

/* `local_epoch`: the visitor's wall-clock time, seconds since 1970. */
EMSCRIPTEN_KEEPALIVE void web_init(double local_epoch)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_buffers(disp, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pointer_cb);

    app_init();
    plat_set_local_epoch((uint32_t)local_epoch);
    app_settings()->sleep_min = 0; /* a demo should not go dark */
    ui_init();
    demo_setup();
}

/* `ms`: time since the page loaded. Returns 1 if the pixels changed. */
EMSCRIPTEN_KEEPALIVE int web_frame(double ms)
{
    last_frame_ms = ms;
    sim_world_step((uint32_t)ms + 1000u);
    lv_timer_handler();
    if (ptr_down && !press_framed) {
        press_framed = true;
        press_frame_ms = ms;
    }
    if (release_pending && ms - press_frame_ms >= MIN_PRESS_MS) {
        ptr_down = false;
        release_pending = false;
        press_framed = false;
    }
    if (!dirty) {
        return 0;
    }
    dirty = false;
    for (int i = 0; i < W * H; i++) {
        uint16_t p = fb[i];
        rgba[4 * i + 0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        rgba[4 * i + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
        rgba[4 * i + 2] = (uint8_t)((p & 0x1F) * 255 / 31);
        rgba[4 * i + 3] = 255;
    }
    return 1;
}

EMSCRIPTEN_KEEPALIVE uint8_t *web_pixels(void) { return rgba; }

EMSCRIPTEN_KEEPALIVE void web_pointer(int x, int y, int down)
{
    ptr_x = x;
    ptr_y = y;
    if (down) {
        ptr_down = true;
        press_framed = false;
        release_pending = false;
    } else if (ptr_down && (!press_framed || last_frame_ms - press_frame_ms < MIN_PRESS_MS)) {
        release_pending = true; /* too short to be seen: keep it down for a few frames */
    } else {
        ptr_down = false;
        press_framed = false;
    }
}

int main(void) { return 0; }
