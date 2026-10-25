/* SPDX-License-Identifier: GPL-3.0-only */
/* Real portable C reorder tests. Payloads/tickets/clocks are synthetic: there
 * is no SRTP, actual retransmission, H264 assembly, decoder or console here.
 * The audio oracle below is the byte-exact frozen0.7.37 reorder implementation
 * from work/cover-opt-verification/final-source.tar.gz, SHA256:
 * edb4b9869329ce38a67cefd685b7755f2fd96aedbbbc6821412e4a8a7478b5a7.
 * Only symbol names are changed by the preprocessor; the file is unchanged. */
#include "live_media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int x4_ref_reorder_init(X4LiveReorder *, uint16_t, uint16_t, uint64_t);
void x4_ref_reorder_free(X4LiveReorder *);
void x4_ref_reorder_reset(X4LiveReorder *);
int x4_ref_reorder_insert(X4LiveReorder *, const uint8_t *, size_t, uint16_t, uint64_t);
uint32_t x4_ref_reorder_abandon_gap_limited(X4LiveReorder *, uint32_t);
uint32_t x4_ref_reorder_abandon_gap(X4LiveReorder *);
const uint8_t *x4_ref_reorder_next_limited(X4LiveReorder *, uint64_t, size_t *, uint32_t *, unsigned *, bool, uint32_t);
const uint8_t *x4_ref_reorder_next_reason(X4LiveReorder *, uint64_t, size_t *, uint32_t *, unsigned *, bool);
const uint8_t *x4_ref_reorder_next(X4LiveReorder *, uint64_t, size_t *, uint32_t *);
#define x4_live_reorder_init x4_ref_reorder_init
#define x4_live_reorder_free x4_ref_reorder_free
#define x4_live_reorder_reset x4_ref_reorder_reset
#define x4_live_reorder_insert x4_ref_reorder_insert
#define x4_live_reorder_abandon_gap_limited x4_ref_reorder_abandon_gap_limited
#define x4_live_reorder_abandon_gap x4_ref_reorder_abandon_gap
#define x4_live_reorder_next_limited x4_ref_reorder_next_limited
#define x4_live_reorder_next_reason x4_ref_reorder_next_reason
#define x4_live_reorder_next x4_ref_reorder_next
#include "fixtures/rtp_reorder_0737_reference.c"
#undef x4_live_reorder_init
#undef x4_live_reorder_free
#undef x4_live_reorder_reset
#undef x4_live_reorder_insert
#undef x4_live_reorder_abandon_gap_limited
#undef x4_live_reorder_abandon_gap
#undef x4_live_reorder_next_limited
#undef x4_live_reorder_next_reason
#undef x4_live_reorder_next

static unsigned cases;
static unsigned long long checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define CASE(x) do { x(); ++cases; printf("PASS %s\n", #x); } while (0)
typedef struct { X4LiveReorder q; X4IngressTicket tickets[128]; } Fixture;

static X4IngressTicket ticket_for(uint64_t sequence)
{
    return (X4IngressTicket){.identity = UINT64_C(0x100000000) | sequence,
        .source = UINT64_C(0x8000006600000088), .extended_sequence = sequence,
        .rtp_timestamp = sequence * 1000, .authenticated_us = sequence * 10,
        .sequence = {.anchor_extended = 100, .previous_high_extended = 99,
            .anchor_us = 900, .transport_epoch = 7, .sequence_epoch = 2,
            .previous_epoch = 1, .anchor_rtp_timestamp = 100000,
            .reason = X4_NACK_SEQUENCE_OMISSION}};
}

static void payload_for(uint8_t *p, size_t size, uint16_t sequence)
{
    for (size_t i = 0; i < size; ++i)
        p[i] = (uint8_t)((sequence >> ((i & 1) * 8)) ^ (uint8_t)(i * 13));
}

static void invariant(const X4LiveReorder *q)
{
    unsigned used = 0;
    uint32_t bytes = 0;
    for (unsigned i = 0; i < q->window; ++i) if (q->slots[i].used) {
        ++used;
        bytes += q->slots[i].size;
        CHECK(q->slots[i].size > 0 && q->slots[i].size <= X4_LIVE_RTP_MAX);
        CHECK(x4_live_reorder_find(q, q->slots[i].sequence) == (int)i);
        if (q->sparse) CHECK((uint16_t)(q->slots[i].sequence - q->expected) < 4096);
        for (unsigned j = i + 1; j < q->window; ++j)
            CHECK(!q->slots[j].used || q->slots[j].sequence != q->slots[i].sequence);
    }
    CHECK(used == q->buffered && bytes == q->resident_bytes);
    CHECK(used <= q->window && bytes <= (uint32_t)q->window * X4_LIVE_RTP_MAX);
    if (q->sparse) CHECK(q->window == 128 && q->max_forward == 4096 && q->tickets != NULL);
}

static int put(Fixture *f, uint16_t sequence, uint64_t extended, size_t size, uint64_t now)
{
    uint8_t bytes[X4_LIVE_RTP_MAX];
    payload_for(bytes, size, sequence);
    X4IngressTicket ticket = ticket_for(extended);
    int rc = x4_live_reorder_insert_ticket(&f->q, bytes, size, sequence, now, &ticket);
    invariant(&f->q);
    return rc;
}

static const uint8_t *take(Fixture *f, uint64_t now, bool force,
    size_t *size, uint32_t *lost, unsigned *reason, X4IngressTicket *ticket)
{
    const uint8_t *result = x4_live_reorder_next_ticket_limited(&f->q, now,
        size, lost, reason, force, x4_live_reorder_advance_bound(&f->q), ticket);
    invariant(&f->q);
    return result;
}

static void init_after(Fixture *f, uint16_t first)
{
    memset(f, 0, sizeof(*f));
    CHECK(x4_live_reorder_init_video(&f->q, 128, 32, 25000, f->tickets) == 0);
    CHECK(put(f, first, first, 16, 1000) == X4_LIVE_REORDER_STORED);
    size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
    CHECK(take(f, 1000, false, &size, &lost, &reason, &ticket) != NULL);
    CHECK(size == 16 && lost == 0 && ticket.extended_sequence == first);
    CHECK(f->q.expected == (uint16_t)(first + 1));
}

static void check_output(const uint8_t *p, size_t size, const X4IngressTicket *ticket,
    uint16_t sequence, uint64_t extended)
{
    uint8_t wanted[X4_LIVE_RTP_MAX];
    payload_for(wanted, size, sequence);
    X4IngressTicket expected = ticket_for(extended);
    CHECK(p != NULL && memcmp(p, wanted, size) == 0);
    CHECK(ticket->identity == expected.identity && ticket->source == expected.source &&
        ticket->extended_sequence == expected.extended_sequence &&
        ticket->rtp_timestamp == expected.rtp_timestamp &&
        ticket->authenticated_us == expected.authenticated_us);
    CHECK(ticket->sequence.anchor_extended == expected.sequence.anchor_extended &&
        ticket->sequence.previous_high_extended == expected.sequence.previous_high_extended &&
        ticket->sequence.anchor_us == expected.sequence.anchor_us &&
        ticket->sequence.transport_epoch == expected.sequence.transport_epoch &&
        ticket->sequence.sequence_epoch == expected.sequence.sequence_epoch &&
        ticket->sequence.previous_epoch == expected.sequence.previous_epoch &&
        ticket->sequence.anchor_rtp_timestamp == expected.sequence.anchor_rtp_timestamp &&
        ticket->sequence.reason == expected.sequence.reason);
}

static void validation_and_memory_contract(void)
{
    Fixture f = {0}; uint8_t bytes[X4_LIVE_RTP_MAX + 1] = {0};
    CHECK(x4_live_reorder_init_video(NULL, 128, 32, 25000, f.tickets) < 0);
    CHECK(x4_live_reorder_init_video(&f.q, 64, 32, 25000, f.tickets) < 0);
    CHECK(x4_live_reorder_init_video(&f.q, 128, 32, 25000, NULL) < 0);
    CHECK(x4_live_reorder_init_video(&f.q, 128, 32, 25000, f.tickets) == 0);
    CHECK(sizeof(X4LiveReorderSlot) == 16 && sizeof(X4IngressTicket) == 88);
    CHECK(X4_LIVE_VIDEO_REORDER_BYTE_LIMIT == 262144);
    CHECK(x4_live_reorder_insert_ticket(&f.q, NULL, 1, 1, 1, NULL) == X4_LIVE_REORDER_INVALID);
    CHECK(x4_live_reorder_insert_ticket(&f.q, bytes, 0, 1, 2, NULL) == X4_LIVE_REORDER_INVALID);
    CHECK(x4_live_reorder_insert_ticket(&f.q, bytes, sizeof(bytes), 1, 3, NULL) == X4_LIVE_REORDER_INVALID);
    CHECK(!f.q.started && f.q.buffered == 0 && f.q.resident_bytes == 0);
    CHECK(f.q.last_insert_reason == X4_REORDER_INSERT_INVALID && f.q.last_insert_us == 3);
    x4_live_reorder_free(&f.q);
    CHECK(f.q.slots == NULL && f.q.data == NULL && f.q.tickets == NULL);
}

static void alias_sequences_keep_exact_payload_tickets(void)
{
    Fixture f; init_after(&f, 100);
    CHECK(put(&f, 102, 102, 29, 2000) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 230, 230, 31, 2001) == X4_LIVE_REORDER_STORED);
    int a = x4_live_reorder_find(&f.q, 102), b = x4_live_reorder_find(&f.q, 230);
    CHECK(a >= 0 && b >= 0 && a != b && (102 & 127) == (230 & 127));
    CHECK(f.q.buffered == 2 && f.q.resident_bytes == 60 && f.q.expected == 101);
    CHECK(x4_live_reorder_span(&f.q) == 129 && x4_live_reorder_advance_bound(&f.q) == 4096);
    CHECK(put(&f, 230, 230 + 65536, 4, 2002) == X4_LIVE_REORDER_DUPLICATE);
    CHECK(f.q.tickets[b].extended_sequence == 230 && f.q.resident_bytes == 60);
    size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
    const uint8_t *p = take(&f, 0, true, &size, &lost, &reason, &ticket);
    CHECK(lost == 1 && reason == 8 && size == 29); check_output(p, size, &ticket, 102, 102);
    CHECK(f.tickets[a].identity == 0);
    p = take(&f, 0, true, &size, &lost, &reason, &ticket);
    CHECK(lost == 127 && reason == 8 && size == 31); check_output(p, size, &ticket, 230, 230);
    CHECK(f.tickets[b].identity == 0 && f.q.expected == 231 && f.q.resident_bytes == 0);
    x4_live_reorder_free(&f.q);
}

static void full_expected_direct_unlocks_without_extra_slot(void)
{
    Fixture f; init_after(&f, 100);
    for (uint16_t seq = 102; seq < 230; ++seq)
        CHECK(put(&f, seq, seq, X4_LIVE_RTP_MAX, 2000 + seq) == X4_LIVE_REORDER_STORED);
    CHECK(f.q.buffered == 128 && f.q.resident_bytes == 262144);
    uint8_t snapshot[128 * X4_LIVE_RTP_MAX]; X4IngressTicket tickets[128];
    memcpy(snapshot, f.q.data, sizeof(snapshot)); memcpy(tickets, f.tickets, sizeof(tickets));
    CHECK(put(&f, 230, 230, 20, 3000) == X4_LIVE_REORDER_FULL);
    CHECK(f.q.last_insert_reason == X4_REORDER_INSERT_CAPACITY && f.q.expected == 101);
    CHECK(memcmp(snapshot, f.q.data, sizeof(snapshot)) == 0);
    CHECK(memcmp(tickets, f.tickets, sizeof(tickets)) == 0);
    CHECK(put(&f, 102, 102, 20, 3001) == X4_LIVE_REORDER_DUPLICATE);
    CHECK(put(&f, 101, 101, X4_LIVE_RTP_MAX, 3002) == X4_LIVE_REORDER_DIRECT);
    CHECK(f.q.expected == 102 && f.q.buffered == 128 && f.q.resident_bytes == 262144);
    CHECK(f.q.direct_packets == 1 && f.q.last_insert_reason == X4_REORDER_INSERT_DIRECT);
    CHECK(f.q.last_insert_us == 3002 && !x4_live_reorder_has(&f.q, 101));
    CHECK(memcmp(snapshot, f.q.data, sizeof(snapshot)) == 0);
    CHECK(memcmp(tickets, f.tickets, sizeof(tickets)) == 0);
    /* DIRECT consumes the already borrowed input; it is not a stored packet.
     * Root's worker integration supplies its immediate AU consumption. */
    for (uint16_t seq = 102; seq < 230; ++seq) {
        size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
        const uint8_t *p = take(&f, 0, false, &size, &lost, &reason, &ticket);
        CHECK(size == X4_LIVE_RTP_MAX && lost == 0 && reason == 0);
        check_output(p, size, &ticket, seq, seq);
    }
    CHECK(f.q.buffered == 0 && f.q.resident_bytes == 0 && f.q.expected == 230);
    x4_live_reorder_free(&f.q);
}

static void wrap_inverted_returns_and_late_duplicates(void)
{
    Fixture f; init_after(&f, 65533);
    CHECK(put(&f, 128, 65664, 10, 2000) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 0, 65536, 11, 2001) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 65535, 65535, 12, 2002) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 65534, 65534, 13, 2003) == X4_LIVE_REORDER_STORED);
    CHECK(x4_live_reorder_find(&f.q, 0) != x4_live_reorder_find(&f.q, 128));
    const uint16_t sequence[] = {65534, 65535, 0, 128};
    const uint64_t extended[] = {65534, 65535, 65536, 65664};
    for (unsigned i = 0; i < 4; ++i) {
        size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
        const uint8_t *p = take(&f, 0, i == 3, &size, &lost, &reason, &ticket);
        CHECK(lost == (i == 3 ? 127u : 0u)); check_output(p, size, &ticket, sequence[i], extended[i]);
    }
    CHECK(put(&f, 0, 65536, 11, 2004) == X4_LIVE_REORDER_LATE);
    CHECK(f.q.expected == 129 && f.q.buffered == 0);
    x4_live_reorder_free(&f.q);
    init_after(&f, 65534);
    for (uint16_t seq = 0; seq < 128; ++seq)
        CHECK(put(&f, seq, 65536u + seq, 16, 3000 + seq) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 65535, 65535, 16, 4000) == X4_LIVE_REORDER_DIRECT);
    CHECK(f.q.expected == 0 && f.q.buffered == 128);
    for (uint16_t seq = 0; seq < 128; ++seq) {
        size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
        const uint8_t *p = take(&f, 0, false, &size, &lost, &reason, &ticket);
        CHECK(lost == 0); check_output(p, size, &ticket, seq, 65536u + seq);
    }
    x4_live_reorder_free(&f.q);
}

static void distance_bound_rebase_budget_and_halfspan(void)
{
    Fixture f; init_after(&f, 100);
    uint16_t seq = (uint16_t)(f.q.expected + 4095);
    CHECK(put(&f, seq, seq, 20, 2000) == X4_LIVE_REORDER_STORED);
    CHECK(f.q.buffered == 1 && f.q.discontinuity_rebases == 0);
    CHECK(x4_live_reorder_span(&f.q) == 4095);
    for (unsigned i = 0; i < 8; ++i) {
        seq = (uint16_t)(f.q.expected + 4096);
        CHECK(put(&f, seq, seq, 20, 2001 + i) == X4_LIVE_REORDER_JUMP);
        CHECK(f.q.discontinuity_rebases == i + 1 && f.q.expected == seq);
        CHECK(f.q.last_insert_reason == X4_REORDER_INSERT_DISTANCE && f.q.buffered == 1);
    }
    uint16_t expected = f.q.expected;
    seq = (uint16_t)(expected + 4096);
    CHECK(put(&f, seq, seq, 20, 2010) == X4_LIVE_REORDER_DISTANCE);
    CHECK(f.q.expected == expected && f.q.buffered == 1 && f.q.discontinuity_rebases == 8);
    seq = (uint16_t)(expected + 32768u);
    CHECK(put(&f, seq, seq, 20, 2011) == X4_LIVE_REORDER_DISCONTINUITY);
    CHECK(f.q.last_insert_reason == X4_REORDER_INSERT_HALFSPAN && f.q.buffered == 1);
    CHECK(put(&f, (uint16_t)(expected - 1), expected - 1u, 20, 2012) == X4_LIVE_REORDER_LATE);
    x4_live_reorder_reset(&f.q);
    CHECK(f.q.discontinuity_rebases == 8 && f.q.resident_bytes == 0);
    CHECK(put(&f, 400, 400, 20, 2013) == X4_LIVE_REORDER_STORED);
    CHECK(put(&f, 4496, 4496, 20, 2014) == X4_LIVE_REORDER_DISTANCE);
    x4_live_reorder_free(&f.q);
    init_after(&f, 100);
    CHECK(f.q.discontinuity_rebases == 0 && f.q.direct_packets == 0);
    x4_live_reorder_free(&f.q);
}

static void nearest_sparse_gap_ordinary_depth_and_wait(void)
{
    Fixture f; init_after(&f, 100);
    CHECK(put(&f, 401, 401, 10, 2000) == X4_LIVE_REORDER_STORED);
    size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
    CHECK(take(&f, 26999, false, &size, &lost, &reason, &ticket) == NULL && lost == 0);
    const uint8_t *p = take(&f, 27000, false, &size, &lost, &reason, &ticket);
    CHECK(lost == 300 && reason == 2); check_output(p, size, &ticket, 401, 401);
    x4_live_reorder_free(&f.q);
    init_after(&f, 100);
    for (uint16_t seq = 401; seq < 433; ++seq)
        CHECK(put(&f, seq, seq, 10, 2000 + seq) == X4_LIVE_REORDER_STORED);
    p = take(&f, 0, false, &size, &lost, &reason, &ticket);
    CHECK(lost == 300 && reason == 1); check_output(p, size, &ticket, 401, 401);
    x4_live_reorder_free(&f.q);
    init_after(&f, 100);
    CHECK(put(&f, 401, 401, 10, 2000) == X4_LIVE_REORDER_STORED);
    CHECK(x4_live_reorder_abandon_gap_limited(&f.q, 50) == 50 && f.q.expected == 151);
    CHECK(x4_live_reorder_abandon_gap_limited(&f.q, 0) == 0 && f.q.expected == 151);
    CHECK(x4_live_reorder_abandon_gap(&f.q) == 250 && f.q.expected == 401);
    CHECK(take(&f, 0, false, &size, &lost, &reason, &ticket) != NULL && lost == 0);
    x4_live_reorder_free(&f.q);
}

static void empty_reset_reinitialization_and_identity_zero(void)
{
    Fixture f; init_after(&f, 100);
    uint8_t p[4] = {1, 2, 3, 4}; X4IngressTicket ticket = ticket_for(102);
    ticket.identity = 0;
    CHECK(x4_live_reorder_insert_ticket(&f.q, p, sizeof(p), 102, 2000, &ticket) == 0);
    int at = x4_live_reorder_find(&f.q, 102);
    CHECK(at >= 0 && memcmp(&ticket, &f.tickets[at], sizeof(ticket)) == 0);
    x4_live_reorder_reset(&f.q);
    CHECK(!f.q.started && f.q.buffered == 0 && f.q.resident_bytes == 0);
    for (unsigned i = 0; i < 128; ++i) {
        X4IngressTicket zero = {0}; CHECK(memcmp(&zero, &f.tickets[i], sizeof(zero)) == 0);
    }
    ticket = ticket_for(70000); ticket.source += 1; ticket.sequence.sequence_epoch += 1;
    CHECK(x4_live_reorder_insert_ticket(&f.q, p, sizeof(p), (uint16_t)70000, 3000, &ticket) == 0);
    size_t size; uint32_t lost; unsigned reason; X4IngressTicket out;
    const uint8_t *result = take(&f, 3000, false, &size, &lost, &reason, &out);
    CHECK(result != NULL && size == 4 && memcmp(result, p, size) == 0);
    CHECK(memcmp(&out, &ticket, sizeof(ticket)) == 0 && lost == 0);
    CHECK(take(&f, 3001, false, &size, &lost, &reason, &out) == NULL && out.source == 0);
    /* Reorder transports opaque provenance; the real binding/grace helpers
     * must reject invalid source/epoch. This is not a source-validation test. */
    x4_live_reorder_free(&f.q);
}

static uint32_t random_next(uint32_t *state)
{ *state = *state * 1664525u + 1013904223u; return *state; }

static void compare_legacy(const X4LiveReorder *a, const X4LiveReorder *b)
{
    CHECK(a->window == b->window && a->depth == b->depth && a->max_wait == b->max_wait);
    CHECK(a->expected == b->expected && a->started == b->started && a->buffered == b->buffered);
    CHECK(!a->sparse && a->tickets == NULL);
    for (unsigned i = 0; i < a->window; ++i) {
        CHECK(a->slots[i].used == b->slots[i].used);
        CHECK(a->slots[i].sequence == b->slots[i].sequence);
        CHECK(a->slots[i].size == b->slots[i].size && a->slots[i].arrival == b->slots[i].arrival);
        if (a->slots[i].used) CHECK(memcmp(a->data + (size_t)i * X4_LIVE_RTP_MAX,
            b->data + (size_t)i * X4_LIVE_RTP_MAX, a->slots[i].size) == 0);
    }
    invariant(a);
}

static void legacy_audio_frozen_source_differential(void)
{
    X4LiveReorder a, b;
    CHECK(x4_live_reorder_init(&a, 64, 8, 30000) == 0);
    CHECK(x4_ref_reorder_init(&b, 64, 8, 30000) == 0);
    uint32_t rng = 0x734a0d10u; uint64_t now = 1000;
    uint8_t bytes[X4_LIVE_RTP_MAX];
    for (unsigned n = 0; n < 30000; ++n) {
        uint32_t value = random_next(&rng);
        now += value % 3001;
        unsigned action = value % 17;
        if (action == 0) {
            x4_live_reorder_reset(&a); x4_ref_reorder_reset(&b);
        } else if (action <= 11) {
            uint16_t seq = (uint16_t)(a.expected + (int)(value % 161) - 30);
            if (!a.started) seq = (uint16_t)value;
            size_t size = 1 + value % X4_LIVE_RTP_MAX;
            payload_for(bytes, size, seq);
            const uint8_t *p = action == 1 ? NULL : bytes;
            if (action == 2) size = 0;
            if (action == 3) size = X4_LIVE_RTP_MAX + 1;
            if (action == 4) for (unsigned i = 0; i < a.window; ++i)
                if (a.slots[i].used) { seq = a.slots[i].sequence; break; }
            CHECK(x4_live_reorder_insert(&a, p, size, seq, now) ==
                x4_ref_reorder_insert(&b, p, size, seq, now));
        } else if (action == 12) {
            uint32_t limit = value % 65;
            CHECK(x4_live_reorder_abandon_gap_limited(&a, limit) ==
                x4_ref_reorder_abandon_gap_limited(&b, limit));
        } else if (action == 13) {
            CHECK(x4_live_reorder_abandon_gap(&a) == x4_ref_reorder_abandon_gap(&b));
        } else {
            size_t as, bs; uint32_t al, bl; unsigned ar, br;
            uint64_t clock = action == 14 ? 0 : now;
            bool force = action == 16;
            uint32_t limit = value % 65;
            const uint8_t *ap = x4_live_reorder_next_limited(&a, clock, &as, &al, &ar, force, limit);
            const uint8_t *bp = x4_ref_reorder_next_limited(&b, clock, &bs, &bl, &br, force, limit);
            CHECK((ap != NULL) == (bp != NULL) && as == bs && al == bl && ar == br);
            if (ap) CHECK(memcmp(ap, bp, as) == 0);
        }
        compare_legacy(&a, &b);
    }
    x4_live_reorder_free(&a); x4_ref_reorder_free(&b);
    printf("ORACLE audio legacy actions=30000 exact_frozen_source=0.7.37\n");
}

static void randomized_sparse_invariants(void)
{
    Fixture f; init_after(&f, 65400);
    uint32_t rng = 0x1284096u; uint64_t now = 2000;
    for (unsigned n = 0; n < 12000; ++n) {
        uint32_t value = random_next(&rng); now += value % 100;
        if ((value & 3u) == 0) {
            size_t size; uint32_t lost; unsigned reason; X4IngressTicket ticket;
            const uint8_t *p = take(&f, 0, true, &size, &lost, &reason, &ticket);
            if (p) check_output(p, size, &ticket, (uint16_t)ticket.extended_sequence, ticket.extended_sequence);
        } else {
            uint16_t seq = (uint16_t)(f.q.expected + value % 512);
            size_t size = 1 + value % X4_LIVE_RTP_MAX;
            int rc = put(&f, seq, seq, size, now);
            CHECK(rc == X4_LIVE_REORDER_STORED || rc == X4_LIVE_REORDER_DUPLICATE ||
                rc == X4_LIVE_REORDER_FULL || rc == X4_LIVE_REORDER_DIRECT);
            /* A DIRECT return is synchronously consumed by this fixture; its
             * caller-owned bytes have not entered the128 resident slots. */
        }
    }
    invariant(&f.q); x4_live_reorder_free(&f.q);
}

int main(void)
{
    CASE(validation_and_memory_contract);
    CASE(alias_sequences_keep_exact_payload_tickets);
    CASE(full_expected_direct_unlocks_without_extra_slot);
    CASE(wrap_inverted_returns_and_late_duplicates);
    CASE(distance_bound_rebase_budget_and_halfspan);
    CASE(nearest_sparse_gap_ordinary_depth_and_wait);
    CASE(empty_reset_reinitialization_and_identity_zero);
    CASE(legacy_audio_frozen_source_differential);
    CASE(randomized_sparse_invariants);
    printf("PASS sparse reorder cases=%u checks=%llu packet_policy_only=1\n", cases, checks);
    return 0;
}
