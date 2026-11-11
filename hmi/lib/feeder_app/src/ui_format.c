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

void ui_fmt_portion(char *buf, size_t n, uint8_t on_ds, uint8_t pulses)
{
    if (pulses == 1) {
        snprintf(buf, n, "1 pulse of %d.%d s", on_ds / 10, on_ds % 10);
    } else {
        snprintf(buf, n, "%d pulses of %d.%d s", pulses, on_ds / 10, on_ds % 10);
    }
}
