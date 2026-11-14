/* Unit tests for the feeder node state machine, driven by fake hardware. */
#include <string.h>
#include <unity.h>

#include "feeder/node.h"

#define MON_0000 1794182400u /* Monday 2026-11-09 00:00:00 */
#define AT(h, m, s) (MON_0000 + (h) * 3600u + (m) * 60u + (s))

static const uint8_t HMI[6] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t OTHER[6] = { 2, 0, 0, 0, 0, 9 };

/* ---- fake hardware ---- */

static struct {
    uint8_t  sent[32][64];
    uint8_t  sent_mac[32][6];
    int      sent_n;
    uint8_t  channel;
    bool     relay;
    int      relay_rises;
    int      cfg_saves, state_saves;
    bool     state_save_fails;
    fd_node_cfg_t   saved_cfg;
    fd_node_state_t saved_state;
    bool     has_clock;
    uint32_t clock_set_to;
    uint32_t failsafe_ms;
    int      ota_calls;
    /* scale under the hopper */
    bool     scale_fitted, scale_ready, scale_steady;
    int32_t  hopper_mg;
    int32_t  mg_per_pulse;
    uint8_t  scale_op;
    uint32_t scale_known;
} hw;

static void hw_send(void *c, const uint8_t mac[6], const void *msg, uint8_t len)
{
    (void)c;
    if (len > 64) {
        return;
    }
    if (hw.sent_n == 32) { /* keep the newest 32 */
        memmove(hw.sent[0], hw.sent[1], sizeof(hw.sent[0]) * 31);
        memmove(hw.sent_mac[0], hw.sent_mac[1], sizeof(hw.sent_mac[0]) * 31);
        hw.sent_n = 31;
    }
    memcpy(hw.sent[hw.sent_n], msg, len);
    memcpy(hw.sent_mac[hw.sent_n], mac, 6);
    hw.sent_n++;
}
static void hw_channel(void *c, uint8_t ch) { (void)c; hw.channel = ch; }
static void hw_relay(void *c, bool on)
{
    (void)c;
    if (on && !hw.relay) {
        hw.relay_rises++;
        hw.hopper_mg -= hw.mg_per_pulse; /* food leaves the hopper */
    }
    hw.relay = on;
}
static void hw_save_cfg(void *c, const fd_node_cfg_t *cfg) { (void)c; hw.saved_cfg = *cfg; hw.cfg_saves++; }
static bool hw_save_state(void *c, const fd_node_state_t *st)
{
    (void)c;
    if (hw.state_save_fails) {
        return false;
    }
    hw.saved_state = *st;
    hw.state_saves++;
    return true;
}
static bool hw_set_clock(void *c, uint32_t e) { (void)c; hw.clock_set_to = e; return hw.has_clock; }
static void hw_failsafe(void *c, uint32_t ms) { (void)c; hw.failsafe_ms = ms; }
static void hw_ota(void *c, const fd_msg_ota_start_t *m) { (void)c; (void)m; hw.ota_calls++; }

static bool hw_scale_found(void *c) { (void)c; return hw.scale_fitted; }
static bool hw_scale_read(void *c, int32_t *mg)
{
    (void)c;
    if (!hw.scale_fitted || !hw.scale_ready || !hw.scale_steady) {
        return false;
    }
    *mg = hw.hopper_mg;
    return true;
}
static bool hw_scale_set(void *c, uint8_t op, uint32_t known)
{
    (void)c;
    hw.scale_op = op;
    hw.scale_known = known;
    if (!hw.scale_fitted) {
        return false;
    }
    hw.scale_ready = op == FD_SCALE_SPAN || hw.scale_ready;
    return true;
}

static const fd_node_io_t IO = { NULL, hw_send, hw_channel, hw_relay, hw_save_cfg, hw_save_state,
                                 hw_set_clock, hw_failsafe, hw_ota, hw_scale_found, hw_scale_read, hw_scale_set };

static fd_node_t node;
static uint32_t ms, epoch;

static void rx(const uint8_t mac[6], const void *msg, uint8_t len);

/* Advance time in 10 ms steps with no traffic from the HMI. */
static void run_quiet(uint32_t dur)
{
    for (uint32_t t = 0; t < dur; t += 10) {
        ms += 10;
        fd_node_tick(&node, ms, epoch + (ms / 1000u));
    }
}

/* Advance time while the HMI keeps answering, as it does when in range. */
static void run(uint32_t dur)
{
    for (uint32_t t = 0; t < dur; t += 10) {
        run_quiet(10);
        if (node.cfg.paired && ms % 5000u == 0) {
            fd_msg_ack_t a;
            fd_hdr_init(&a.hdr, FD_MSG_ACK, 0);
            a.acked_type = FD_MSG_STATUS;
            a.code = 0;
            a.event_id = 0;
            rx(HMI, &a, sizeof(a));
        }
    }
}
static uint32_t now(void) { return epoch + ms / 1000u; }

static void rx(const uint8_t mac[6], const void *msg, uint8_t len)
{
    fd_node_rx(&node, ms, now(), mac, msg, len);
}

/* Last message of a type the node sent, or NULL. */
static const uint8_t *last_sent(uint8_t type)
{
    for (int i = hw.sent_n - 1; i >= 0; i--) {
        if (((const fd_hdr_t *)hw.sent[i])->type == type) {
            return hw.sent[i];
        }
    }
    return NULL;
}
static int count_sent(uint8_t type)
{
    int n = 0;
    for (int i = 0; i < hw.sent_n; i++) {
        n += ((const fd_hdr_t *)hw.sent[i])->type == type;
    }
    return n;
}
static void clear_sent(void) { hw.sent_n = 0; }

static void boot(const fd_node_cfg_t *cfg, const fd_node_state_t *st, bool time_valid)
{
    fd_node_init(&node, &IO, cfg, st, 0, 2, true, time_valid, ms, now());
}

static void pair(void)
{
    fd_msg_pair_t p;
    memset(&p, 0, sizeof(p));
    fd_hdr_init(&p.hdr, FD_MSG_PAIR, 1);
    p.node_id = 3;
    strcpy(p.name, "Tank");
    rx(HMI, &p, sizeof(p));
}

static void feed_now(uint8_t seq, uint8_t on_ds, uint8_t pulses)
{
    fd_msg_feed_now_t f;
    fd_hdr_init(&f.hdr, FD_MSG_FEED_NOW, seq);
    f.on_ds = on_ds;
    f.pulses = pulses;
    rx(HMI, &f, sizeof(f));
}

static void set_schedule(uint8_t hour, uint8_t minute)
{
    fd_msg_sched_set_t m;
    memset(&m, 0, sizeof(m));
    fd_hdr_init(&m.hdr, FD_MSG_SCHED_SET, 5);
    m.rev = 7;
    m.limits = (fd_limits_t){ 5, 0, 0 };
    m.count = 1;
    m.entries[0] = (fd_sched_entry_t){ hour, minute, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 1 };
    rx(HMI, &m, (uint8_t)FD_SCHED_SET_LEN(1));
}

void setUp(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.has_clock = true;
    ms = 5000;
    epoch = AT(7, 0, 0) - 5;
    fd_node_cfg_t cfg;
    fd_node_state_t st = { 0 };
    fd_node_default_config(&cfg);
    boot(&cfg, &st, true);
}
void tearDown(void) {}

/* ---- pairing and link ---- */

static void test_blank_node_announces_itself_on_every_channel(void)
{
    uint16_t seen = 0;
    for (int i = 0; i < 40; i++) {
        run(FD_NODE_HOP_PERIOD_MS);
        seen |= (uint16_t)(1u << hw.channel);
    }
    TEST_ASSERT_EQUAL_HEX16(0x3FFE, seen); /* channels 1..13 */
    TEST_ASSERT_TRUE(count_sent(FD_MSG_HELLO) > 10);
    TEST_ASSERT_EQUAL(FD_LED_PAIRING, fd_node_led(&node));
    TEST_ASSERT_FALSE(hw.relay);
}

static void test_pair_is_stored_with_the_channel_it_arrived_on(void)
{
    run(FD_NODE_HOP_PERIOD_MS * 4);
    uint8_t ch = hw.channel;
    pair();
    TEST_ASSERT_EQUAL_UINT8(1, hw.saved_cfg.paired);
    TEST_ASSERT_EQUAL_UINT8(3, hw.saved_cfg.node_id);
    TEST_ASSERT_EQUAL_UINT8(ch, hw.saved_cfg.channel);
    TEST_ASSERT_EQUAL_MEMORY(HMI, hw.saved_cfg.hmi_mac, 6);
    TEST_ASSERT_NOT_NULL(last_sent(FD_MSG_ACK));
    run(100);
    TEST_ASSERT_NOT_NULL(last_sent(FD_MSG_STATUS)); /* reports in straight away */
    TEST_ASSERT_EQUAL_UINT8(ch, hw.channel);        /* and stops hopping */
}

static void test_pair_is_ignored_outside_pairing_mode(void)
{
    pair();
    clear_sent();
    fd_msg_pair_t p;
    memset(&p, 0, sizeof(p));
    fd_hdr_init(&p.hdr, FD_MSG_PAIR, 2);
    p.node_id = 9;
    rx(OTHER, &p, sizeof(p)); /* someone else tries to take the node over */
    TEST_ASSERT_EQUAL_UINT8(3, node.cfg.node_id);
    TEST_ASSERT_EQUAL_MEMORY(HMI, node.cfg.hmi_mac, 6);
    TEST_ASSERT_NULL(last_sent(FD_MSG_ACK));
}

static void test_commands_from_a_stranger_are_ignored(void)
{
    pair();
    clear_sent();
    fd_msg_feed_now_t f;
    fd_hdr_init(&f.hdr, FD_MSG_FEED_NOW, 1);
    f.on_ds = 10;
    f.pulses = 1;
    rx(OTHER, &f, sizeof(f));
    run(200);
    TEST_ASSERT_FALSE(hw.relay);
    TEST_ASSERT_EQUAL(0, hw.relay_rises);
    TEST_ASSERT_NULL(last_sent(FD_MSG_ACK));
}

static void test_lost_link_searches_channels_and_recovers(void)
{
    pair();
    uint8_t home = hw.channel;
    run_quiet(FD_NODE_LINK_TIMEOUT_MS + 2000); /* HMI goes quiet */
    TEST_ASSERT_EQUAL(FD_LED_SEARCHING, fd_node_led(&node));
    uint16_t seen = 0;
    for (int i = 0; i < 20; i++) {
        run_quiet(FD_NODE_HOP_PERIOD_MS);
        seen |= (uint16_t)(1u << hw.channel);
    }
    TEST_ASSERT_EQUAL_HEX16(0x3FFE, seen);

    /* The HMI answers on whatever channel the node is on now. */
    uint8_t found = hw.channel;
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, 1);
    a.acked_type = FD_MSG_STATUS;
    a.code = 0;
    a.event_id = 0;
    rx(HMI, &a, sizeof(a));
    TEST_ASSERT_EQUAL(FD_LED_OK, fd_node_led(&node));
    run(3000);
    TEST_ASSERT_EQUAL_UINT8(found, hw.channel); /* stays put */
    if (found != home) {
        TEST_ASSERT_EQUAL_UINT8(found, hw.saved_cfg.channel); /* and remembers it */
    }
}

static void test_unpair_forgets_the_hmi(void)
{
    pair();
    fd_msg_simple_t u;
    fd_hdr_init(&u.hdr, FD_MSG_UNPAIR, 4);
    rx(HMI, &u, sizeof(u));
    TEST_ASSERT_EQUAL_UINT8(0, hw.saved_cfg.paired);
    TEST_ASSERT_EQUAL(FD_LED_PAIRING, fd_node_led(&node));
    clear_sent();
    feed_now(9, 10, 1); /* the old HMI no longer has any say */
    TEST_ASSERT_EQUAL(0, hw.relay_rises);
}

/* ---- feeding ---- */

static void test_manual_feed_runs_the_relay_and_reports(void)
{
    pair();
    clear_sent();
    feed_now(10, 10, 2); /* 2 x 1.0 s, 0.5 s gap */
    TEST_ASSERT_TRUE(hw.relay);
    TEST_ASSERT_EQUAL_UINT32(2500 + FD_NODE_FAILSAFE_MARGIN_MS, hw.failsafe_ms);
    TEST_ASSERT_EQUAL(1, hw.state_saves); /* recorded before the relay ran */
    const fd_msg_ack_t *a = (const fd_msg_ack_t *)last_sent(FD_MSG_ACK);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, a->code);
    run(3000);
    TEST_ASSERT_FALSE(hw.relay);
    TEST_ASSERT_EQUAL(2, hw.relay_rises);
    TEST_ASSERT_EQUAL_UINT32(0, hw.failsafe_ms); /* disarmed */
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, e->result);
    TEST_ASSERT_EQUAL_UINT8(FD_SOURCE_MANUAL, e->source);
    TEST_ASSERT_EQUAL_UINT8(2, e->pulses_done);
    TEST_ASSERT_EQUAL_UINT16(2000, e->on_ms_total);
}

static void test_retransmitted_feed_request_feeds_once(void)
{
    pair();
    feed_now(20, 5, 1);
    run(100);
    feed_now(20, 5, 1); /* the HMI did not hear the ACK and repeats */
    feed_now(20, 5, 1);
    run(2000);
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    TEST_ASSERT_EQUAL_UINT16(1, node.st.guard.feeds_today);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
}

static void test_reused_sequence_number_later_is_a_new_feed(void)
{
    pair();
    node.cfg.limits.cooldown_s = 0;
    node.cfg.limits.daily_max_on_s = 0;
    feed_now(20, 5, 1);
    run(2000);
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    run(FD_NODE_FEED_DEDUPE_MS + 1000); /* the HMI rebooted or its counter wrapped */
    feed_now(20, 5, 1);
    run(2000);
    TEST_ASSERT_EQUAL(2, hw.relay_rises);
    TEST_ASSERT_EQUAL_UINT16(2, node.st.guard.feeds_today);
}

static void test_feed_is_refused_when_it_cannot_be_recorded(void)
{
    pair();
    clear_sent();
    hw.state_save_fails = true;
    feed_now(40, 10, 1);
    TEST_ASSERT_FALSE(hw.relay);
    TEST_ASSERT_EQUAL(0, hw.relay_rises);
    TEST_ASSERT_EQUAL_UINT16(0, node.st.guard.feeds_today);
    TEST_ASSERT_EQUAL_UINT32(0, node.st.guard.on_ms_today);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_STORAGE, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    hw.state_save_fails = false; /* storage recovers: the same node can feed again */
    run(100);
    feed_now(41, 10, 1);
    TEST_ASSERT_TRUE(hw.relay);
}

static void test_cooldown_refuses_and_says_so(void)
{
    pair();
    feed_now(30, 5, 1);
    run(2000);
    feed_now(31, 5, 1);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_COOLDOWN, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    run(2000);
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
}

static void test_request_longer_than_the_hard_limit_is_clamped(void)
{
    pair();
    feed_now(40, 255, 1);
    run(FD_HARD_MAX_ON_DS * 100 - 20);
    TEST_ASSERT_TRUE(hw.relay);
    run(40);
    TEST_ASSERT_FALSE(hw.relay);
}

static void test_failsafe_aborts_and_reports(void)
{
    pair();
    clear_sent();
    feed_now(50, 20, 1);
    run(300);
    hw.relay = false;               /* the independent timer cut the relay */
    fd_node_failsafe_fired(&node);
    run(100);
    TEST_ASSERT_FALSE(hw.relay);
    run(4000);
    TEST_ASSERT_EQUAL(1, hw.relay_rises); /* it does not switch back on */
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_ABORTED, e->result);
}

static void test_events_are_resent_until_acknowledged(void)
{
    pair();
    feed_now(60, 5, 1);
    run(600);
    clear_sent();
    run(FD_NODE_EVENT_RETRY_MS * 3 + 100);
    TEST_ASSERT_TRUE(count_sent(FD_MSG_FEED_EVENT) >= 3);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    fd_msg_ack_t a;
    fd_hdr_init(&a.hdr, FD_MSG_ACK, e->hdr.seq);
    a.acked_type = FD_MSG_FEED_EVENT;
    a.code = 0;
    a.event_id = e->event_id;
    rx(HMI, &a, sizeof(a));
    clear_sent();
    run(FD_NODE_EVENT_RETRY_MS * 3);
    TEST_ASSERT_EQUAL(0, count_sent(FD_MSG_FEED_EVENT));
}

/* ---- schedule ---- */

static void test_scheduled_feed_fires_once_in_its_minute(void)
{
    pair();
    set_schedule(7, 1);
    TEST_ASSERT_EQUAL_UINT16(7, hw.saved_cfg.sched_rev);
    run(50000);
    TEST_ASSERT_EQUAL(0, hw.relay_rises); /* 07:00:xx, not yet */
    run(70000);                           /* through 07:01 */
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    run(120000);
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0, e->source);
}

static void test_reboot_in_the_same_minute_does_not_feed_again(void)
{
    pair();
    set_schedule(7, 1);
    run(62000); /* 07:01:0x: fed */
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    /* Power cut and restart with what was saved to flash. */
    fd_node_cfg_t cfg = hw.saved_cfg;
    fd_node_state_t st = hw.saved_state;
    boot(&cfg, &st, true);
    run(20000); /* still 07:01 */
    TEST_ASSERT_EQUAL(1, hw.relay_rises);
    TEST_ASSERT_EQUAL_UINT8(1, node.cfg.paired); /* and it is still paired */
    TEST_ASSERT_EQUAL_UINT8(1, node.cfg.count);
}

static void test_no_scheduled_feed_until_the_clock_is_set(void)
{
    fd_node_cfg_t cfg;
    fd_node_state_t st = { 0 };
    fd_node_default_config(&cfg);
    boot(&cfg, &st, false); /* clock chip lost power */
    pair();
    set_schedule(7, 1);
    run(130000);
    TEST_ASSERT_EQUAL(0, hw.relay_rises);
    TEST_ASSERT_EQUAL(FD_LED_NO_TIME, fd_node_led(&node));

    fd_msg_time_sync_t t;
    fd_hdr_init(&t.hdr, FD_MSG_TIME_SYNC, 8);
    t.local_epoch = AT(9, 59, 50);
    rx(HMI, &t, sizeof(t));
    TEST_ASSERT_EQUAL_UINT32(AT(9, 59, 50), hw.clock_set_to);
    TEST_ASSERT_EQUAL(FD_LED_OK, fd_node_led(&node));
    /* Manual feeds work without a valid clock too. */
}

static void test_time_sync_without_a_clock_chip_is_refused(void)
{
    pair();
    hw.has_clock = false;
    fd_msg_time_sync_t t;
    fd_hdr_init(&t.hdr, FD_MSG_TIME_SYNC, 8);
    t.local_epoch = AT(9, 0, 0);
    rx(HMI, &t, sizeof(t));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_TIME_INVALID, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
}

static void test_bad_schedule_is_rejected_whole(void)
{
    pair();
    set_schedule(7, 30);
    fd_msg_sched_set_t m;
    memset(&m, 0, sizeof(m));
    fd_hdr_init(&m.hdr, FD_MSG_SCHED_SET, 6);
    m.rev = 8;
    m.count = 2;
    m.entries[0] = (fd_sched_entry_t){ 8, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 1 };
    m.entries[1] = (fd_sched_entry_t){ 25, 0, FD_ENTRY_ENABLED | FD_DOW_ALL, 10, 1 }; /* hour 25 */
    rx(HMI, &m, (uint8_t)FD_SCHED_SET_LEN(2));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_BAD_REQUEST, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    TEST_ASSERT_EQUAL_UINT16(7, node.cfg.sched_rev); /* the old one stays */
    TEST_ASSERT_EQUAL_UINT8(1, node.cfg.count);
}

/* ---- scale ---- */

static void fit_scale(void)
{
    hw.scale_fitted = hw.scale_ready = hw.scale_steady = true;
    hw.hopper_mg = 100000;
    hw.mg_per_pulse = 550;
}

static void test_without_a_scale_feeds_are_not_measured(void)
{
    pair();
    clear_sent();
    feed_now(80, 5, 2);
    run(3000);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX32(FD_NOT_MEASURED, e->measured_mg);
    const fd_msg_status_t *s = (const fd_msg_status_t *)last_sent(FD_MSG_STATUS);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT8(0, s->flags & (FD_FLAG_SCALE | FD_FLAG_SCALE_FOUND));
}

static void test_scale_measures_what_the_hopper_lost(void)
{
    fit_scale();
    pair();
    clear_sent();
    feed_now(81, 5, 3); /* 3 pulses of 550 mg: 3 x 0.5 s on, 2 x 0.5 s gap */
    run(2600);          /* relay done at 2.5 s; the event waits for the weighing */
    TEST_ASSERT_NULL(last_sent(FD_MSG_FEED_EVENT));
    run(FD_NODE_SETTLE_MS + 200);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(1650, e->measured_mg);
    TEST_ASSERT_EQUAL_UINT8(3, e->pulses_done);
    TEST_ASSERT_EQUAL_UINT32(1650, node.st.fed_mg_today);
    TEST_ASSERT_EQUAL_UINT32(1650, hw.saved_state.fed_mg_today);

    run(FD_NODE_STATUS_PERIOD_MS);
    const fd_msg_status_t *s = (const fd_msg_status_t *)last_sent(FD_MSG_STATUS);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->flags & FD_FLAG_SCALE);
    TEST_ASSERT_TRUE(s->flags & FD_FLAG_SCALE_FOUND);
    TEST_ASSERT_EQUAL_INT32(100000 - 1650, s->hopper_mg);
    TEST_ASSERT_EQUAL_UINT32(1650, s->fed_mg_today);
}

static void test_a_jam_is_measured_as_nothing(void)
{
    fit_scale();
    hw.mg_per_pulse = 0; /* the mechanism moves but no food falls */
    pair();
    feed_now(82, 5, 2);
    run(5000);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, e->result);
    TEST_ASSERT_EQUAL_UINT32(0, e->measured_mg);
}

static void test_no_second_feed_while_weighing(void)
{
    fit_scale();
    pair();
    fd_limits_t none = { 5, 0, 0 };
    node.cfg.limits = none; /* no cooldown, so only the weighing can refuse */
    feed_now(83, 5, 1);
    run(600); /* relay done at 0.5 s, weighing until 2.0 s */
    feed_now(84, 5, 1);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_BUSY, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    run(2000);
    feed_now(85, 5, 1);
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
}

static void test_unsteady_scale_gives_up_and_reports_unmeasured(void)
{
    fit_scale();
    pair();
    clear_sent();
    feed_now(86, 5, 1);
    hw.scale_steady = false; /* the hopper keeps swinging */
    run(FD_NODE_WEIGH_TIMEOUT_MS + 1000);
    const fd_msg_feed_event_t *e = (const fd_msg_feed_event_t *)last_sent(FD_MSG_FEED_EVENT);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_HEX32(FD_NOT_MEASURED, e->measured_mg);
    TEST_ASSERT_EQUAL_UINT32(0, node.st.fed_mg_today);
}

static void test_scale_commands_reach_the_hardware(void)
{
    pair();
    fd_msg_scale_set_t m;
    fd_hdr_init(&m.hdr, FD_MSG_SCALE_SET, 90);
    m.op = FD_SCALE_ZERO;
    m.known_mg = 0;
    rx(HMI, &m, sizeof(m)); /* no scale fitted */
    TEST_ASSERT_EQUAL_UINT8(FD_RES_BAD_REQUEST, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);

    hw.scale_fitted = true;
    hw.scale_steady = true;
    rx(HMI, &m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    m.hdr.seq = 91;
    m.op = FD_SCALE_SPAN;
    m.known_mg = 100000;
    rx(HMI, &m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT8(FD_SCALE_SPAN, hw.scale_op);
    TEST_ASSERT_EQUAL_UINT32(100000, hw.scale_known);

    m.hdr.seq = 92;
    m.op = 9; /* unknown operation */
    rx(HMI, &m, sizeof(m));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_BAD_REQUEST, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    rx(OTHER, &m, sizeof(m)); /* and never from a stranger */
}

/* ---- firmware update ---- */

static void test_update_waits_for_a_running_feed(void)
{
    pair();
    feed_now(70, 10, 1);
    fd_msg_ota_start_t o;
    memset(&o, 0, sizeof(o));
    fd_hdr_init(&o.hdr, FD_MSG_OTA_START, 71);
    strcpy(o.ssid, "AP");
    strcpy(o.pass, "password");
    rx(HMI, &o, sizeof(o));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_BUSY, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    run(2000);
    TEST_ASSERT_EQUAL(0, hw.ota_calls);

    o.hdr.seq = 72;
    rx(HMI, &o, sizeof(o));
    TEST_ASSERT_EQUAL_UINT8(FD_RES_OK, ((const fd_msg_ack_t *)last_sent(FD_MSG_ACK))->code);
    run(50);
    TEST_ASSERT_EQUAL(1, hw.ota_calls);
    TEST_ASSERT_FALSE(hw.relay);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blank_node_announces_itself_on_every_channel);
    RUN_TEST(test_pair_is_stored_with_the_channel_it_arrived_on);
    RUN_TEST(test_pair_is_ignored_outside_pairing_mode);
    RUN_TEST(test_commands_from_a_stranger_are_ignored);
    RUN_TEST(test_lost_link_searches_channels_and_recovers);
    RUN_TEST(test_unpair_forgets_the_hmi);
    RUN_TEST(test_manual_feed_runs_the_relay_and_reports);
    RUN_TEST(test_retransmitted_feed_request_feeds_once);
    RUN_TEST(test_reused_sequence_number_later_is_a_new_feed);
    RUN_TEST(test_feed_is_refused_when_it_cannot_be_recorded);
    RUN_TEST(test_cooldown_refuses_and_says_so);
    RUN_TEST(test_request_longer_than_the_hard_limit_is_clamped);
    RUN_TEST(test_failsafe_aborts_and_reports);
    RUN_TEST(test_events_are_resent_until_acknowledged);
    RUN_TEST(test_scheduled_feed_fires_once_in_its_minute);
    RUN_TEST(test_reboot_in_the_same_minute_does_not_feed_again);
    RUN_TEST(test_no_scheduled_feed_until_the_clock_is_set);
    RUN_TEST(test_time_sync_without_a_clock_chip_is_refused);
    RUN_TEST(test_bad_schedule_is_rejected_whole);
    RUN_TEST(test_without_a_scale_feeds_are_not_measured);
    RUN_TEST(test_scale_measures_what_the_hopper_lost);
    RUN_TEST(test_a_jam_is_measured_as_nothing);
    RUN_TEST(test_no_second_feed_while_weighing);
    RUN_TEST(test_unsteady_scale_gives_up_and_reports_unmeasured);
    RUN_TEST(test_scale_commands_reach_the_hardware);
    RUN_TEST(test_update_waits_for_a_running_feed);
    return UNITY_END();
}
