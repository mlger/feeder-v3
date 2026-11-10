#include <string.h>
#include <unity.h>

#include "feeder/core.h"

void setUp(void) {}
void tearDown(void) {}

/* 2026-11-09 was a Monday. 1794182400 = 2026-11-09 00:00:00. */
#define MON_0000 1794182400u
#define AT(day_off, h, m, s) (MON_0000 + (day_off) * 86400u + (h) * 3600u + (m) * 60u + (s))

static const fd_limits_t LIM = { .gap_ds = 5, .cooldown_s = 600, .daily_max_on_s = 10 };

/* ---- time ---- */

static void test_time_split(void)
{
    fd_tod_t t;
    fd_time_split(0, &t);
    TEST_ASSERT_EQUAL_UINT8(4, t.dow); /* Thursday */
    fd_time_split(AT(0, 7, 30, 15), &t);
    TEST_ASSERT_EQUAL_UINT8(1, t.dow);
    TEST_ASSERT_EQUAL_UINT8(7, t.hour);
    TEST_ASSERT_EQUAL_UINT8(30, t.minute);
    TEST_ASSERT_EQUAL_UINT8(15, t.second);
    fd_time_split(AT(6, 23, 59, 59), &t);
    TEST_ASSERT_EQUAL_UINT8(0, t.dow); /* Sunday */
}

static void test_civil_round_trip(void)
{
    fd_civil_t c;
    fd_civil_from_epoch(AT(0, 7, 30, 15), &c);
    TEST_ASSERT_EQUAL_UINT16(2026, c.year);
    TEST_ASSERT_EQUAL_UINT8(11, c.month);
    TEST_ASSERT_EQUAL_UINT8(9, c.day);
    TEST_ASSERT_EQUAL_UINT8(7, c.hour);
    TEST_ASSERT_EQUAL_UINT8(30, c.minute);
    TEST_ASSERT_EQUAL_UINT8(15, c.second);
    TEST_ASSERT_EQUAL_UINT8(1, c.dow);
    TEST_ASSERT_EQUAL_UINT32(AT(0, 7, 30, 15), fd_civil_to_epoch(&c));

    /* Leap day and year boundaries. */
    fd_civil_t leap = { 2028, 2, 29, 23, 59, 59, 0 };
    uint32_t e = fd_civil_to_epoch(&leap);
    fd_civil_from_epoch(e + 1, &c);
    TEST_ASSERT_EQUAL_UINT8(3, c.month);
    TEST_ASSERT_EQUAL_UINT8(1, c.day);
    fd_civil_from_epoch(0, &c);
    TEST_ASSERT_EQUAL_UINT16(1970, c.year);
    TEST_ASSERT_EQUAL_UINT8(1, c.month);
    TEST_ASSERT_EQUAL_UINT8(1, c.day);

    /* Every day for 80 years survives a round trip. */
    for (uint32_t d = 0; d < 80u * 366u; d++) {
        uint32_t t = d * 86400u + 12345u;
        fd_civil_from_epoch(t, &c);
        TEST_ASSERT_EQUAL_UINT32(t, fd_civil_to_epoch(&c));
    }
    TEST_ASSERT_EQUAL_UINT8(29, fd_days_in_month(2028, 2));
    TEST_ASSERT_EQUAL_UINT8(28, fd_days_in_month(2026, 2));
    TEST_ASSERT_EQUAL_UINT8(28, fd_days_in_month(2100, 2));
    TEST_ASSERT_EQUAL_UINT8(31, fd_days_in_month(2026, 12));
}

/* ---- schedule ---- */

static void test_sched_due(void)
{
    fd_sched_entry_t e[4] = {
        { 7, 30, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 1 },
        { 7, 30, FD_DOW_ALL, 10, 1 },                    /* disabled */
        { 18, 0, FD_ENTRY_ENABLED | (1 << 1), 10, 2 },   /* Mondays only */
        { 7, 30, FD_ENTRY_ENABLED | (1 << 6), 10, 1 },   /* Saturdays only */
    };
    TEST_ASSERT_EQUAL_HEX16(0x0001, fd_sched_due(e, 4, AT(0, 7, 30, 0)));
    TEST_ASSERT_EQUAL_HEX16(0x0001, fd_sched_due(e, 4, AT(0, 7, 30, 59)));
    TEST_ASSERT_EQUAL_HEX16(0x0000, fd_sched_due(e, 4, AT(0, 7, 31, 0)));
    TEST_ASSERT_EQUAL_HEX16(0x0004, fd_sched_due(e, 4, AT(0, 18, 0, 5)));
    TEST_ASSERT_EQUAL_HEX16(0x0000, fd_sched_due(e, 4, AT(1, 18, 0, 5))); /* Tuesday */
    TEST_ASSERT_EQUAL_HEX16(0x0009, fd_sched_due(e, 4, AT(5, 7, 30, 0))); /* Saturday */
    TEST_ASSERT_EQUAL_HEX16(0x0000, fd_sched_due(e, 0, AT(0, 7, 30, 0)));
}

static void test_sched_ignores_invalid_entry(void)
{
    fd_sched_entry_t e[2] = {
        { 7, 30, FD_ENTRY_ENABLED | FD_DOW_ALL, 0, 1 }, /* zero on-time */
        { 7, 30, FD_ENTRY_ENABLED | FD_DOW_ALL, 5, 0 }, /* zero pulses */
    };
    TEST_ASSERT_EQUAL_HEX16(0, fd_sched_due(e, 2, AT(0, 7, 30, 0)));
    fd_sched_entry_t bad = { 24, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, 5, 1 };
    TEST_ASSERT_FALSE(fd_sched_entry_valid(&bad));
}

/* ---- guard ---- */

static void test_guard_clamps_to_hard_limits(void)
{
    fd_guard_t g = { 0 };
    fd_limits_t lim = { 0 };
    uint8_t on = 255, pulses = 255;
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &lim, AT(0, 8, 0, 0), &on, &pulses));
    TEST_ASSERT_EQUAL_UINT8(FD_HARD_MAX_ON_DS, on);
    TEST_ASSERT_EQUAL_UINT8(FD_HARD_MAX_PULSES, pulses);
}

static void test_guard_rejects_empty_feed(void)
{
    fd_guard_t g = { 0 };
    uint8_t on = 0, pulses = 1;
    TEST_ASSERT_EQUAL(FD_RES_BAD_REQUEST, fd_guard_check(&g, &LIM, AT(0, 8, 0, 0), &on, &pulses));
    on = 5; pulses = 0;
    TEST_ASSERT_EQUAL(FD_RES_BAD_REQUEST, fd_guard_check(&g, &LIM, AT(0, 8, 0, 0), &on, &pulses));
}

static void test_guard_cooldown(void)
{
    fd_guard_t g = { 0 };
    uint8_t on = 10, pulses = 1;
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &LIM, AT(0, 8, 0, 0), &on, &pulses));
    fd_guard_commit(&g, AT(0, 8, 0, 0), 1000);
    TEST_ASSERT_EQUAL(FD_RES_COOLDOWN, fd_guard_check(&g, &LIM, AT(0, 8, 9, 59), &on, &pulses));
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &LIM, AT(0, 8, 10, 0), &on, &pulses));
}

static void test_guard_clock_moved_back_restarts_cooldown(void)
{
    fd_guard_t g = { 0 };
    uint8_t on = 10, pulses = 1;
    fd_guard_commit(&g, AT(0, 8, 0, 0), 1000);
    /* Clock set back one hour: must not feed again straight away. */
    TEST_ASSERT_EQUAL(FD_RES_COOLDOWN, fd_guard_check(&g, &LIM, AT(0, 7, 0, 0), &on, &pulses));
    TEST_ASSERT_EQUAL(FD_RES_COOLDOWN, fd_guard_check(&g, &LIM, AT(0, 7, 9, 0), &on, &pulses));
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &LIM, AT(0, 7, 10, 0), &on, &pulses));
}

static void test_guard_daily_cap_and_rollover(void)
{
    fd_guard_t g = { 0 };
    fd_limits_t lim = { .gap_ds = 5, .cooldown_s = 0, .daily_max_on_s = 10 };
    uint8_t on = 20, pulses = 2; /* 4 s per feed */
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &lim, AT(0, 8, i, 0), &on, &pulses));
        fd_guard_commit(&g, AT(0, 8, i, 0), 4000);
    }
    /* 8 s used, 4 s more would be 12 s > 10 s cap. */
    TEST_ASSERT_EQUAL(FD_RES_DAILY_CAP, fd_guard_check(&g, &lim, AT(0, 9, 0, 0), &on, &pulses));
    TEST_ASSERT_EQUAL_UINT16(2, g.feeds_today);
    /* A smaller feed that still fits is allowed. */
    uint8_t on2 = 20, p2 = 1;
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &lim, AT(0, 9, 0, 0), &on2, &p2));
    /* Next day the counters reset. */
    TEST_ASSERT_EQUAL(FD_RES_OK, fd_guard_check(&g, &lim, AT(1, 0, 0, 0), &on, &pulses));
    TEST_ASSERT_EQUAL_UINT32(0, g.on_ms_today);
    TEST_ASSERT_EQUAL_UINT16(0, g.feeds_today);
}

/* ---- pulse sequencer ---- */

static void test_pulse_sequence(void)
{
    fd_pulse_t p;
    fd_pulse_start(&p, 1000, 10, 2, 5); /* 1.0 s on, 0.5 s gap, twice */
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, 1000));
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, 1999));
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 2000));
    TEST_ASSERT_TRUE(p.active);
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 2499));
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, 2500));
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, 3499));
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 3500));
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_EQUAL_UINT8(2, p.pulses_done);
    TEST_ASSERT_EQUAL_UINT32(2000, p.on_ms_total);
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 99999));
    TEST_ASSERT_EQUAL_UINT32(2500, fd_pulse_duration_ms(10, 2, 5));
}

static void test_pulse_clamps_and_survives_millis_wrap(void)
{
    fd_pulse_t p;
    uint32_t start = 0xFFFFFF00u; /* millis() about to wrap */
    fd_pulse_start(&p, start, 255, 1, 0);
    TEST_ASSERT_EQUAL_UINT16(FD_HARD_MAX_ON_DS * 100, p.on_ms);
    TEST_ASSERT_EQUAL_UINT16(FD_HARD_MIN_GAP_DS * 100, p.gap_ms);
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, start + 100));
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, start + FD_HARD_MAX_ON_DS * 100 - 1)); /* wrapped */
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, start + FD_HARD_MAX_ON_DS * 100));
    TEST_ASSERT_FALSE(p.active);
}

static void test_pulse_abort_and_empty(void)
{
    fd_pulse_t p;
    fd_pulse_start(&p, 0, 10, 3, 5);
    TEST_ASSERT_TRUE(fd_pulse_tick(&p, 10));
    fd_pulse_abort(&p);
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 20));
    TEST_ASSERT_FALSE(p.active);

    fd_pulse_start(&p, 0, 0, 3, 5);
    TEST_ASSERT_FALSE(p.active);
    TEST_ASSERT_FALSE(fd_pulse_tick(&p, 1));
    TEST_ASSERT_EQUAL_UINT32(0, fd_pulse_duration_ms(0, 3, 5));
}

/* ---- protocol ---- */

static void test_proto_validate(void)
{
    fd_msg_feed_now_t m;
    fd_hdr_init(&m.hdr, FD_MSG_FEED_NOW, 7);
    m.on_ds = 10; m.pulses = 1;
    TEST_ASSERT_EQUAL_UINT8(FD_MSG_FEED_NOW, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m) - 1));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m) + 1));
    m.hdr.version++;
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    m.hdr.version--; m.hdr.magic = 0;
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    m.hdr.magic = FD_PROTO_MAGIC; m.hdr.type = 99;
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate(NULL, 10));
}

static void test_proto_ota_start(void)
{
    fd_msg_ota_start_t m;
    memset(&m, 0, sizeof(m));
    fd_hdr_init(&m.hdr, FD_MSG_OTA_START, 1);
    strcpy(m.ssid, "AIO-Feeder-OTA");
    strcpy(m.pass, "abcd1234efgh");
    TEST_ASSERT_EQUAL_UINT8(FD_MSG_OTA_START, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m) - 1));
    /* Strings that are not terminated are rejected. */
    memset(m.ssid, 'A', sizeof(m.ssid));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    m.ssid[sizeof(m.ssid) - 1] = '\0';
    memset(m.pass, 'B', sizeof(m.pass));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
}

static void test_proto_sched_set_length(void)
{
    fd_msg_sched_set_t m;
    memset(&m, 0, sizeof(m));
    fd_hdr_init(&m.hdr, FD_MSG_SCHED_SET, 1);
    m.count = 3;
    TEST_ASSERT_EQUAL_UINT8(FD_MSG_SCHED_SET, fd_proto_validate((uint8_t *)&m, FD_SCHED_SET_LEN(3)));
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, FD_SCHED_SET_LEN(2)));
    m.count = FD_MAX_ENTRIES + 1;
    TEST_ASSERT_EQUAL_UINT8(0, fd_proto_validate((uint8_t *)&m, sizeof(m)));
    m.count = 0;
    TEST_ASSERT_EQUAL_UINT8(FD_MSG_SCHED_SET, fd_proto_validate((uint8_t *)&m, FD_SCHED_SET_LEN(0)));
    /* Largest message must fit one ESP-NOW frame. */
    TEST_ASSERT_LESS_OR_EQUAL(250, sizeof(fd_msg_sched_set_t));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_time_split);
    RUN_TEST(test_civil_round_trip);
    RUN_TEST(test_sched_due);
    RUN_TEST(test_sched_ignores_invalid_entry);
    RUN_TEST(test_guard_clamps_to_hard_limits);
    RUN_TEST(test_guard_rejects_empty_feed);
    RUN_TEST(test_guard_cooldown);
    RUN_TEST(test_guard_clock_moved_back_restarts_cooldown);
    RUN_TEST(test_guard_daily_cap_and_rollover);
    RUN_TEST(test_pulse_sequence);
    RUN_TEST(test_pulse_clamps_and_survives_millis_wrap);
    RUN_TEST(test_pulse_abort_and_empty);
    RUN_TEST(test_proto_validate);
    RUN_TEST(test_proto_ota_start);
    RUN_TEST(test_proto_sched_set_length);
    return UNITY_END();
}
