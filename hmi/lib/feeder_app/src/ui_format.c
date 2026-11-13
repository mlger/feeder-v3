/* Text formatting for the HMI. */
#include "ui_internal.h"

#include <stdio.h>

const char *const ui_dow3[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
const char *const ui_mon3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void ui_fmt_when(char *buf, size_t n, uint32_t epoch, bool relative)
{
    if (epoch == 0) {
        snprintf(buf, n, "never");
        return;
    }
    fd_civil_t c;
    fd_civil_from_epoch(epoch, &c);
    if (relative && plat_time_valid()) {
        int32_t dd = (int32_t)(epoch / 86400u) - (int32_t)(plat_local_epoch() / 86400u);
        if (dd >= -1 && dd <= 1) {
            snprintf(buf, n, "%s %02d:%02d", dd == 0 ? "today" : dd < 0 ? "yesterday" : "tomorrow",
                     c.hour, c.minute);
            return;
        }
    }
    snprintf(buf, n, "%s %02d %s %02d:%02d", ui_dow3[c.dow], c.day, ui_mon3[c.month - 1], c.hour, c.minute);
}

void ui_fmt_ds(char *buf, size_t n, int ds) { snprintf(buf, n, "%d.%d s", ds / 10, ds % 10); }

void ui_fmt_days(char *buf, size_t n, uint8_t mask)
{
    mask &= FD_DOW_ALL;
    if (mask == FD_DOW_ALL) {
        snprintf(buf, n, "Every day");
    } else if (mask == 0x3E) {
        snprintf(buf, n, "Weekdays");
    } else if (mask == 0x41) {
        snprintf(buf, n, "Weekends");
    } else if (mask == 0) {
        snprintf(buf, n, "No days");
    } else {
        size_t pos = 0;
        buf[0] = '\0';
        for (int d = 0; d < 7 && pos + 5 < n; d++) {
            if (mask & (1u << d)) {
                pos += (size_t)snprintf(buf + pos, n - pos, "%s%s", pos ? " " : "", ui_dow3[d]);
            }
        }
    }
}

/* What an amount in grams is run as, and what that really gives, e.g.
 * "= 4 pulses, gives about 2.4 g" or "= 4.2 s, gives about 2.5 g". */
void ui_fmt_plan_note(char *buf, size_t n, int idx, uint32_t asked_mg, const char *verb)
{
    uint8_t on, p;
    char g[16];
    app_plan(idx, asked_mg, &on, &p);
    ui_fmt_grams(g, sizeof(g), app_mg(idx, on, p));
    const char *more = asked_mg > app_max_target_mg(idx) ? "More than one feed can give: " : "= ";
    if (app_plan_by_rate(idx)) {
        unsigned total = (unsigned)on * p;
        if (p == 1) {
            snprintf(buf, n, "%s%u.%u s, %s %s", more, total / 10, total % 10, verb, g);
        } else {
            snprintf(buf, n, "%s%u x %u.%u s, %s %s", more, p, on / 10, on % 10, verb, g);
        }
    } else {
        snprintf(buf, n, "%s%u pulse%s, %s %s", more, p, p == 1 ? "" : "s", verb, g);
    }
}

void ui_fmt_portion(char *buf, size_t n, uint8_t on_ds, uint8_t pulses)
{
    if (pulses == 1) {
        snprintf(buf, n, "1 pulse of %d.%d s", on_ds / 10, on_ds % 10);
    } else {
        snprintf(buf, n, "%d pulses of %d.%d s", pulses, on_ds / 10, on_ds % 10);
    }
}

void ui_fmt_grams(char *buf, size_t n, uint32_t mg)
{
    /* Round to a tenth of a gram first, so 1.96 g shows as 2.0 g. */
    uint32_t tenths = (mg + 50u) / 100u;
    snprintf(buf, n, "%u.%u g", (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
}

void ui_fmt_amount(char *buf, size_t n, int idx, uint8_t on_ds, uint8_t pulses)
{
    uint32_t mg = app_mg(idx, on_ds, pulses);
    if (mg) {
        ui_fmt_grams(buf, n, mg);
    } else {
        ui_fmt_portion(buf, n, on_ds, pulses);
    }
}
