/*
 * PC tests for the Event sender (box_sender). Each test starts the session
 * (BOOT) or finds it open, adds Events, feeds ACKs and ticks, then checks
 * only the BOOT and EVT lines it wrote.
 */
#include <string.h>

#include "box_sender.h"
#include "test_harness.h"

#define MAX_LINES 256
#define LINE_LEN 64
#define MAX_DROPS 16

typedef struct {
    char lines[MAX_LINES][LINE_LEN];
    int count;
    uint32_t dropped_seqs[MAX_DROPS];
    int drop_count;
} Recorder;

static void record_line(void *context, const char *line)
{
    Recorder *r = context;
    if (r->count < MAX_LINES) {
        strncpy(r->lines[r->count], line, LINE_LEN - 1);
        r->lines[r->count][LINE_LEN - 1] = '\0';
    }
    r->count++;
}

static void record_drop(void *context, uint32_t seq, BoxEvent event)
{
    Recorder *r = context;
    (void)event;
    if (r->drop_count < MAX_DROPS) {
        r->dropped_seqs[r->drop_count] = seq;
    }
    r->drop_count++;
}

typedef struct {
    BoxSender sender;
    Recorder recorder;
} Rig;

/* A sender just after power-up: no session yet. */
static void power_up(Rig *rig)
{
    BoxSenderOutputs outputs;
    memset(rig, 0, sizeof *rig);
    outputs.context = &rig->recorder;
    outputs.write_line = record_line;
    outputs.on_dropped = record_drop;
    box_sender_init(&rig->sender, outputs);
}

/* A sender whose session is open (BOOT sent and ACKed at 0 ms), with the
 * BOOT line forgotten, so a test sees only the lines it causes. */
static void start(Rig *rig)
{
    power_up(rig);
    box_sender_start(&rig->sender, 0);
    box_sender_ack(&rig->sender, BOX_BOOT_SEQ, 0);
    memset(&rig->recorder, 0, sizeof rig->recorder);
}

#define CHECK_LINE(rig, index, expected)                                  \
    do {                                                                  \
        CHECK((index) < MAX_LINES && (rig)->recorder.count > (index));    \
        if ((index) < MAX_LINES && (rig)->recorder.count > (index) &&     \
            strcmp((rig)->recorder.lines[(index)], (expected)) != 0) {    \
            printf("    %s:%d: line %d: expected \"%s\", got \"%s\"\n",   \
                   __FILE__, __LINE__, (index), (expected),               \
                   (rig)->recorder.lines[(index)]);                       \
            test_current_failed = 1;                                      \
        }                                                                 \
    } while (0)

/* ---- Tests: the session (BOOT) ------------------------------------------- */

static void test_nothing_is_sent_before_the_session_starts(void)
{
    Rig rig;
    power_up(&rig);

    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_tick(&rig.sender, 5000 + 5 * BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(0, rig.recorder.count);
}

static void test_starting_the_session_sends_boot(void)
{
    Rig rig;
    power_up(&rig);

    box_sender_start(&rig.sender, 2000);

    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "BOOT\n");
}

static void test_starting_the_session_again_sends_nothing(void)
{
    Rig rig;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);

    box_sender_start(&rig.sender, 2500);
    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 2600);
    box_sender_start(&rig.sender, 2700);

    CHECK_EQ_INT(1, rig.recorder.count);
}

static void test_boot_is_resent_until_its_ack(void)
{
    Rig rig;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);

    /* BOOT or its ACK,0 is lost twice. */
    box_sender_tick(&rig.sender, 2000 + BOX_SENDER_RESEND_MS - 10);
    CHECK_EQ_INT(1, rig.recorder.count);
    box_sender_tick(&rig.sender, 2000 + BOX_SENDER_RESEND_MS);
    box_sender_tick(&rig.sender, 2000 + 2 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 1, "BOOT\n");
    CHECK_LINE(&rig, 2, "BOOT\n");

    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 4010);
    box_sender_tick(&rig.sender, 4010 + 10 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
}

static void test_events_wait_for_the_session_then_leave_in_order(void)
{
    Rig rig;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 2100);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 2200);

    /* Unacked BOOT: it, not an Event, is what gets resent. */
    box_sender_tick(&rig.sender, 2000 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "BOOT\n");

    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 3010);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,1,DELIVERED\n");

    box_sender_ack(&rig.sender, 1, 3020);
    CHECK_EQ_INT(4, rig.recorder.count);
    CHECK_LINE(&rig, 3, "EVT,2,COLLECTED\n");
}

static void test_a_late_ack_from_before_the_reboot_is_ignored_while_booting(void)
{
    Rig rig;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 2100);

    box_sender_ack(&rig.sender, 1, 2200);   /* the old session's ACK,1 */
    CHECK_EQ_INT(1, rig.recorder.count);    /* no EVT: still booting */

    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 2300);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
    /* Event 1 is still in flight: the old ACK didn't complete it. */
    box_sender_tick(&rig.sender, 2300 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,1,DELIVERED\n");
}

static void test_a_full_outbox_before_the_session_opens_drops_the_oldest_event(void)
{
    Rig rig;
    uint32_t i;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);
    /* Nothing ACKs BOOT, so no Event has been sent: keep the newest ones. */
    for (i = 0; i < BOX_SENDER_CAPACITY + 1u; i++) {
        box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, 2100u + 100u * i);
    }
    CHECK_EQ_INT(1, rig.recorder.drop_count);
    CHECK_EQ_INT(1, rig.recorder.dropped_seqs[0]);

    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 4000);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,2,OPENED_EMPTY\n");
}

static void test_ack_says_whether_it_was_accepted(void)
{
    Rig rig;
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 2100);

    CHECK(!box_sender_ack(&rig.sender, 1, 2200));             /* booting: stale */
    CHECK(box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 2300));   /* session open */
    CHECK(!box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 2310));  /* repeated ACK,0 */
    CHECK(!box_sender_ack(&rig.sender, 2, 2320));             /* not in flight */
    CHECK(box_sender_ack(&rig.sender, 1, 2330));              /* Event 1 done */
    CHECK(!box_sender_ack(&rig.sender, 1, 2340));             /* duplicate */
}

static void test_ack_0_once_the_session_is_open_is_ignored(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 5010);   /* a repeated ACK,0 */
    box_sender_tick(&rig.sender, 5000 + BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
}

/*
 * The ESP32's side of the contract (product.md section 5), as a model to
 * run the sender's lines through: BOOT starts a new session (forget the
 * last accepted seq); an EVT with a seq at or below the last accepted one
 * in this session is a duplicate. Returns how many Events it accepted.
 */
typedef struct {
    uint32_t last_seq;   /* 0: none accepted in this session */
    int accepted;
} Esp32Model;

static void esp32_read(Esp32Model *esp, const Recorder *r, int from, int to)
{
    int i;
    for (i = from; i < to && i < MAX_LINES; i++) {
        const char *line = r->lines[i];
        if (strcmp(line, "BOOT\n") == 0) {
            esp->last_seq = 0;
        } else if (strncmp(line, "EVT,", 4) == 0) {
            uint32_t seq = 0;
            const char *p;
            for (p = line + 4; *p >= '0' && *p <= '9'; p++) {
                seq = seq * 10u + (uint32_t)(*p - '0');
            }
            if (seq > esp->last_seq) {
                esp->last_seq = seq;
                esp->accepted++;
            }
        }
    }
}

static void test_a_reboot_after_exactly_one_event_is_not_taken_for_a_duplicate(void)
{
    Rig rig;
    Esp32Model esp = {0, 0};
    int before_reboot;

    /* Session 1: one Delivery, resent once (its ACK lost), then ACKed. */
    power_up(&rig);
    box_sender_start(&rig.sender, 2000);
    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 2010);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 3000);
    box_sender_tick(&rig.sender, 3000 + BOX_SENDER_RESEND_MS);
    box_sender_ack(&rig.sender, 1, 4010);
    before_reboot = rig.recorder.count;

    /* Reboot 2 s later (well within a heartbeat). Session 2: BOOT's first
     * send is lost on the wire, then a Collection. */
    box_sender_init(&rig.sender, rig.sender.outputs);
    box_sender_start(&rig.sender, 2000);
    rig.recorder.lines[before_reboot][0] = '\0';   /* the ESP32 never read it */
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 2500);
    box_sender_tick(&rig.sender, 2000 + BOX_SENDER_RESEND_MS);
    box_sender_ack(&rig.sender, BOX_BOOT_SEQ, 3010);
    box_sender_tick(&rig.sender, 3010 + BOX_SENDER_RESEND_MS);   /* ACK,1 lost */
    box_sender_ack(&rig.sender, 1, 4020);

    esp32_read(&esp, &rig.recorder, 0, rig.recorder.count);
    CHECK_EQ_INT(2, esp.accepted);   /* each Event once: the resends were dropped */
    CHECK_LINE(&rig, rig.recorder.count - 1, "EVT,1,COLLECTED\n");
}

/* ---- Tests: Events ------------------------------------------------------- */

static void test_an_event_is_sent_at_once_with_sequence_number_1(void)
{
    Rig rig;
    start(&rig);

    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "EVT,1,DELIVERED\n");
}

/* Tick every 10 ms from `from_ms` up to and including `to_ms`. Counts
 * elapsed time, so it is safe across the millisecond wraparound. */
static void tick_through(Rig *rig, uint32_t from_ms, uint32_t to_ms)
{
    uint32_t now_ms = from_ms;
    for (;;) {
        box_sender_tick(&rig->sender, now_ms);
        if (now_ms == to_ms) {
            break;
        }
        now_ms += 10u;
    }
}

static void test_an_unacked_event_is_resent_every_resend_interval(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5000);

    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS - 10);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, 5000 + BOX_SENDER_RESEND_MS, 5000 + 3 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(4, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,COLLECTED\n");
    CHECK_LINE(&rig, 2, "EVT,1,COLLECTED\n");
    CHECK_LINE(&rig, 3, "EVT,1,COLLECTED\n");
}

static void test_an_event_is_not_sent_again_after_its_ack(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    box_sender_ack(&rig.sender, 1, 5020);
    tick_through(&rig, 5020, 5020 + 10 * BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(1, rig.recorder.count);
}

static void test_an_ack_for_another_sequence_number_is_ignored(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);

    box_sender_ack(&rig.sender, 2, 5020);
    box_sender_ack(&rig.sender, 0, 5030);
    tick_through(&rig, 5030, 5000 + BOX_SENDER_RESEND_MS);

    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");
}

static void test_events_leave_in_order_one_at_a_time(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_OCCUPIED, 6000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 7000);

    /* Only the oldest goes out while it waits for its ACK. */
    CHECK_EQ_INT(1, rig.recorder.count);
    CHECK_LINE(&rig, 0, "EVT,1,DELIVERED\n");

    box_sender_ack(&rig.sender, 1, 7010);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,2,OPENED_OCCUPIED\n");

    box_sender_ack(&rig.sender, 2, 7020);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,3,COLLECTED\n");

    box_sender_ack(&rig.sender, 3, 7030);
    tick_through(&rig, 7030, 7030 + 5 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
}

static void test_the_next_event_gets_the_full_resend_interval(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);
    tick_through(&rig, 5000, 5900);

    box_sender_ack(&rig.sender, 1, 5900);
    tick_through(&rig, 5900, 5900 + BOX_SENDER_RESEND_MS - 10);
    CHECK_EQ_INT(2, rig.recorder.count);

    tick_through(&rig, 5900 + BOX_SENDER_RESEND_MS, 5900 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

static void test_a_lost_ack_is_survived_by_resending_until_the_next_ack(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5500);

    /* The ESP32's ACK,1 for the first send is lost: no ack call. */
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,DELIVERED\n");

    /* The ESP32 ACKs the resend (as a duplicate it ignores but ACKs). */
    box_sender_ack(&rig.sender, 1, 6020);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

static void test_a_duplicate_ack_is_harmless(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS);   /* resent once */

    box_sender_ack(&rig.sender, 1, 6010);   /* ACK of the first send */
    box_sender_ack(&rig.sender, 1, 6020);   /* ACK of the resend */

    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
    /* Event 2 is still in flight, so it is still resent. */
    tick_through(&rig, 6020, 6010 + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(4, rig.recorder.count);
    CHECK_LINE(&rig, 3, "EVT,2,COLLECTED\n");
}

static void test_resending_keeps_time_across_the_millisecond_wraparound(void)
{
    Rig rig;
    uint32_t sent_at_ms = 0xFFFFFFFFu - 495u;   /* a multiple of 10 ms */
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, sent_at_ms);

    tick_through(&rig, sent_at_ms, sent_at_ms + BOX_SENDER_RESEND_MS - 10u);
    CHECK_EQ_INT(1, rig.recorder.count);

    tick_through(&rig, sent_at_ms + BOX_SENDER_RESEND_MS, sent_at_ms + BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(2, rig.recorder.count);
    CHECK_LINE(&rig, 1, "EVT,1,OPENED_EMPTY\n");
}

/* A tick stamped a few ms before the ACK that sent the next Event (the main
 * loop read its clock before the ACK arrived) is not a resend: the Event
 * has only just been sent. */
static void test_a_tick_stamped_just_before_the_send_does_not_resend(void)
{
    Rig rig;
    start(&rig);
    box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000);
    box_sender_add(&rig.sender, BOX_EVENT_COLLECTED, 5100);

    box_sender_ack(&rig.sender, 1, 6003);   /* sends Event 2 at 6003 */
    box_sender_tick(&rig.sender, 6000);
    CHECK_EQ_INT(2, rig.recorder.count);

    tick_through(&rig, 6010, 6003 + BOX_SENDER_RESEND_MS - 3);
    CHECK_EQ_INT(2, rig.recorder.count);
    tick_through(&rig, 6003 + BOX_SENDER_RESEND_MS + 7, 6003 + BOX_SENDER_RESEND_MS + 7);
    CHECK_EQ_INT(3, rig.recorder.count);
    CHECK_LINE(&rig, 2, "EVT,2,COLLECTED\n");
}

/* Ack the Event in flight and return the last line written (the next
 * Event's, if one was sent), or "" if nothing has been written. */
static const char *ack_in_flight(Rig *rig, uint32_t seq, uint32_t now_ms)
{
    box_sender_ack(&rig->sender, seq, now_ms);
    if (rig->recorder.count < 1 || rig->recorder.count > MAX_LINES) {
        return "";
    }
    return rig->recorder.lines[rig->recorder.count - 1];
}

static void test_a_full_outbox_drops_the_oldest_event_waiting_behind_the_one_in_flight(void)
{
    Rig rig;
    uint32_t i;
    start(&rig);
    /* The link is down: nothing is ACKed. Fill the outbox, then one more. */
    for (i = 0; i < BOX_SENDER_CAPACITY + 1u; i++) {
        box_sender_add(&rig.sender, BOX_EVENT_OPENED_EMPTY, 5000u + 100u * i);
    }

    CHECK_EQ_INT(1, rig.recorder.drop_count);
    CHECK_EQ_INT(2, rig.recorder.dropped_seqs[0]);

    /* The link comes back: Event 1 (in flight), then 3 up to the newest,
     * in order, each sent once. */
    CHECK_LINE(&rig, 0, "EVT,1,OPENED_EMPTY\n");
    CHECK(strcmp(ack_in_flight(&rig, 1, 9000), "EVT,3,OPENED_EMPTY\n") == 0);
    CHECK(strcmp(ack_in_flight(&rig, 3, 9010), "EVT,4,OPENED_EMPTY\n") == 0);
    for (i = 4; i <= BOX_SENDER_CAPACITY + 1u; i++) {
        box_sender_ack(&rig.sender, i, 9020u + i);
    }
    CHECK_EQ_INT(BOX_SENDER_CAPACITY, rig.recorder.count);
    CHECK_LINE(&rig, (int)BOX_SENDER_CAPACITY - 1, "EVT,9,OPENED_EMPTY\n");
    tick_through(&rig, 9100, 9100 + 5 * BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(BOX_SENDER_CAPACITY, rig.recorder.count);
}

static void test_the_event_in_flight_is_never_dropped(void)
{
    Rig rig;
    uint32_t i;
    start(&rig);
    for (i = 0; i < 3u * BOX_SENDER_CAPACITY; i++) {
        box_sender_add(&rig.sender, BOX_EVENT_DELIVERED, 5000u + 100u * i);
    }
    tick_through(&rig, 5000, 5000 + BOX_SENDER_RESEND_MS * 4);

    CHECK_EQ_INT(2 * BOX_SENDER_CAPACITY, rig.recorder.drop_count);
    CHECK_LINE(&rig, rig.recorder.count - 1, "EVT,1,DELIVERED\n");
}

/* ---- Wired as the firmware wires it (main.c) ------------------------------ */

/* The core's outputs share box_lines' context, so on_event and
 * on_box_status_changed find the sender here, as main.c finds its static
 * one. */
static BoxSender *wired_sender;

static void wired_on_event(void *context, BoxEvent event, uint32_t now_ms)
{
    (void)context;
    box_sender_add(wired_sender, event, now_ms);
}

static void wired_box_status_changed(void *context, BoxStatus status, uint32_t now_ms)
{
    box_lines_on_box_status_changed(context, status, now_ms);
    box_sender_start(wired_sender, now_ms);
}

static BoxReadings readings(bool lid_open, bool parcel_present)
{
    BoxReadings r;
    r.lid_open = lid_open;
    r.parcel_present = parcel_present;
    return r;
}

typedef struct {
    Rig rig;            /* the sender, and the recorder every line goes to */
    BoxLines lines;
    BoxCore core;
} Firmware;

static uint32_t run_firmware(Firmware *fw, uint32_t now_ms, BoxReadings r,
                             uint32_t duration_ms)
{
    uint32_t elapsed_ms;
    for (elapsed_ms = 0; elapsed_ms < duration_ms; elapsed_ms += 10u) {
        now_ms += 10u;
        box_core_tick(&fw->core, now_ms, r);
        box_lines_tick(&fw->lines, now_ms);
        box_sender_tick(&fw->rig.sender, now_ms);
    }
    return now_ms;
}

static void test_wired_to_the_core_boot_follows_the_first_sts_and_a_delivery_is_resent_until_acked(void)
{
    static Firmware fw;
    BoxLinesOutput lines_output;
    BoxCoreOutputs core_outputs;
    uint32_t now_ms = 0;
    power_up(&fw.rig);
    wired_sender = &fw.rig.sender;
    lines_output.context = &fw.rig.recorder;
    lines_output.write_line = record_line;
    box_lines_init(&fw.lines, lines_output);
    core_outputs = box_lines_core_outputs(&fw.lines);
    core_outputs.on_event = wired_on_event;
    core_outputs.on_box_status_changed = wired_box_status_changed;
    box_core_init(&fw.core, core_outputs, now_ms, readings(false, false));

    /* Nothing at all until the starting state is known, then STS and BOOT. */
    now_ms = run_firmware(&fw, now_ms, readings(false, false), BOX_SETTLE_TIME_MS - 10);
    CHECK_EQ_INT(0, fw.rig.recorder.count);
    now_ms = run_firmware(&fw, now_ms, readings(false, false), 110);
    CHECK_EQ_INT(2, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 0, "STS,EMPTY,LID_CLOSED\n");
    CHECK_LINE(&fw.rig, 1, "BOOT\n");
    box_sender_ack(&fw.rig.sender, BOX_BOOT_SEQ, now_ms);

    now_ms = run_firmware(&fw, now_ms, readings(true, false), 1000);
    now_ms = run_firmware(&fw, now_ms, readings(false, true), BOX_SETTLE_TIME_MS + 100);
    CHECK_EQ_INT(6, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 4, "STS,OCCUPIED,LID_CLOSED\n");
    CHECK_LINE(&fw.rig, 5, "EVT,1,DELIVERED\n");

    now_ms = run_firmware(&fw, now_ms, readings(false, true), BOX_SENDER_RESEND_MS);
    CHECK_EQ_INT(7, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 6, "EVT,1,DELIVERED\n");

    box_sender_ack(&fw.rig.sender, 1, now_ms);
    /* From here on only the STS heartbeat; no second BOOT. */
    run_firmware(&fw, now_ms, readings(false, true), BOX_STS_HEARTBEAT_MS);
    CHECK_EQ_INT(8, fw.rig.recorder.count);
    CHECK_LINE(&fw.rig, 7, "STS,OCCUPIED,LID_CLOSED\n");
}

int main(void)
{
    RUN_TEST(test_nothing_is_sent_before_the_session_starts);
    RUN_TEST(test_starting_the_session_sends_boot);
    RUN_TEST(test_starting_the_session_again_sends_nothing);
    RUN_TEST(test_boot_is_resent_until_its_ack);
    RUN_TEST(test_events_wait_for_the_session_then_leave_in_order);
    RUN_TEST(test_a_late_ack_from_before_the_reboot_is_ignored_while_booting);
    RUN_TEST(test_a_full_outbox_before_the_session_opens_drops_the_oldest_event);
    RUN_TEST(test_ack_says_whether_it_was_accepted);
    RUN_TEST(test_ack_0_once_the_session_is_open_is_ignored);
    RUN_TEST(test_a_reboot_after_exactly_one_event_is_not_taken_for_a_duplicate);
    RUN_TEST(test_an_event_is_sent_at_once_with_sequence_number_1);
    RUN_TEST(test_an_unacked_event_is_resent_every_resend_interval);
    RUN_TEST(test_an_event_is_not_sent_again_after_its_ack);
    RUN_TEST(test_an_ack_for_another_sequence_number_is_ignored);
    RUN_TEST(test_events_leave_in_order_one_at_a_time);
    RUN_TEST(test_the_next_event_gets_the_full_resend_interval);
    RUN_TEST(test_a_lost_ack_is_survived_by_resending_until_the_next_ack);
    RUN_TEST(test_a_duplicate_ack_is_harmless);
    RUN_TEST(test_resending_keeps_time_across_the_millisecond_wraparound);
    RUN_TEST(test_a_tick_stamped_just_before_the_send_does_not_resend);
    RUN_TEST(test_a_full_outbox_drops_the_oldest_event_waiting_behind_the_one_in_flight);
    RUN_TEST(test_the_event_in_flight_is_never_dropped);
    RUN_TEST(test_wired_to_the_core_boot_follows_the_first_sts_and_a_delivery_is_resent_until_acked);
    return TEST_SUMMARY();
}
