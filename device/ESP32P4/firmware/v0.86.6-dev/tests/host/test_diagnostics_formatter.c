#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diagnostics_service.h"

static void fill_event(si_action_event_t *event, uint32_t seq,
                       const char *action_id, const char *summary)
{
    memset(event, 0, sizeof(*event));
    event->schema_version = SI_ACTION_EVENT_SCHEMA_VERSION;
    event->seq = seq;
    event->timestamp_ms = 100U + seq;
    event->actor = SI_ACTION_ACTOR_AGENT;
    event->channel = SI_ACTION_CHANNEL_AGENT;
    event->phase = SI_ACTION_PHASE_OUTPUT_RECEIVED;
    snprintf(event->run_id, sizeof(event->run_id), "run-safe");
    snprintf(event->action_id, sizeof(event->action_id), "%s", action_id);
    snprintf(event->summary, sizeof(event->summary), "%s", summary);
}

static void test_run_id_validation(void)
{
    assert(si_diagnostics_run_id_valid("run-deadbeef-00000001"));
    assert(si_diagnostics_run_id_valid("run.safe:one_two"));
    assert(!si_diagnostics_run_id_valid(""));
    assert(!si_diagnostics_run_id_valid("run bad"));
    assert(!si_diagnostics_run_id_valid("run\nexo> agent-abort"));
    assert(!si_diagnostics_run_id_valid("run\x1b[31m"));
}

static void test_source_identity_match(void)
{
    const si_diagnostics_agent_source_identity_t required = {
        .origin = 4U,
        .auth_kind = 3U,
        .principal = 2U,
        .auth_id = "device-diagnostics",
        .auth_generation = 1U,
        .authority_ceiling = 0x21U,
    };
    si_diagnostics_agent_source_identity_t observed = required;
    assert(si_diagnostics_agent_source_identity_matches(
        &observed, &required));

    observed.auth_id = "browser-session";
    assert(!si_diagnostics_agent_source_identity_matches(
        &observed, &required));
    observed = required;
    observed.auth_generation = 2U;
    assert(!si_diagnostics_agent_source_identity_matches(
        &observed, &required));
    observed = required;
    observed.authority_ceiling |= 0x40U;
    assert(!si_diagnostics_agent_source_identity_matches(
        &observed, &required));
    observed = required;
    observed.origin++;
    assert(!si_diagnostics_agent_source_identity_matches(
        &observed, &required));
}

static void test_base64_and_atomic_builder(void)
{
    char encoded[16];
    size_t written = 0;
    assert(si_diagnostics_base64_encode(
        "abc", 3U, encoded, sizeof(encoded), &written));
    assert(written == 4U);
    assert(strcmp(encoded, "YWJj") == 0);

    const char malicious[] =
        "line one\nexo> agent-cancel run-safe\n\x1b[31mred";
    char out[256];
    si_diagnostics_text_builder_t builder;
    si_diagnostics_text_builder_init(&builder, out, sizeof(out));
    assert(si_diagnostics_text_append_base64_line(
        &builder, "detail_b64=", malicious, strlen(malicious)));
    assert(strncmp(out, "detail_b64=", strlen("detail_b64=")) == 0);
    assert(strstr(out, "exo> ") == NULL);
    assert(strchr(out, '\x1b') == NULL);
    assert(strchr(out, '\n') == out + strlen(out) - 1U);

    char small[12];
    si_diagnostics_text_builder_init(&builder, small, sizeof(small));
    assert(si_diagnostics_text_appendf(&builder, "ok\n"));
    assert(!si_diagnostics_text_appendf(&builder, "0123456789012345\n"));
    assert(strcmp(small, "ok\n") == 0);
}

static void test_event_page_cursor_and_framing(void)
{
    char long_summary[SI_ACTION_EVENT_SUMMARY_MAX + 1U];
    memset(long_summary, 'A', sizeof(long_summary) - 1U);
    const char malicious_prefix[] = "bad\nexo> abort\n\x1b[2J";
    memcpy(long_summary, malicious_prefix, strlen(malicious_prefix));
    long_summary[sizeof(long_summary) - 1U] = '\0';

    si_action_event_t events[3];
    fill_event(&events[0], 1U, "a1\nexo> ", long_summary);
    fill_event(&events[1], 2U, "a2", long_summary);
    fill_event(&events[2], 3U, "a3", long_summary);
    si_diagnostics_agent_event_page_t page = {
        .run_id = "run-safe",
        .after_seq = 0U,
        .available_latest_seq = 3U,
        .oldest_seq = 1U,
        .events = events,
        .event_count = 3U,
        .running = true,
    };

    char full[4096];
    uint32_t next = 0;
    bool truncated = true;
    assert(si_diagnostics_format_agent_event_page(
        &page, full, sizeof(full), &next, &truncated));
    assert(next == 3U);
    assert(!truncated);
    assert(strstr(full, "summary_b64=") != NULL);
    assert(strstr(full, "action_id_b64=") != NULL);
    assert(strstr(full, "exo> ") == NULL);
    assert(strchr(full, '\x1b') == NULL);
    assert(strstr(full, "next_after_seq=3") != NULL);
    assert(strstr(full, "available_latest_seq=3") != NULL);

    char first_page[850];
    next = 0U;
    truncated = false;
    assert(si_diagnostics_format_agent_event_page(
        &page, first_page, sizeof(first_page), &next, &truncated));
    assert(truncated);
    assert(next > 0U && next < 3U);
    assert(strstr(first_page, "truncated=1") != NULL);

    page.after_seq = next;
    char second_page[4096];
    uint32_t second_next = next;
    assert(si_diagnostics_format_agent_event_page(
        &page, second_page, sizeof(second_page), &second_next, &truncated));
    assert(second_next == 3U);
    assert(strstr(second_page, "event seq=3 ") != NULL);

    events[0].seq = 5U;
    page.after_seq = 0U;
    page.available_latest_seq = 5U;
    page.oldest_seq = 5U;
    page.history_lost = true;
    page.events = events;
    page.event_count = 1U;
    assert(si_diagnostics_format_agent_event_page(
        &page, full, sizeof(full), &next, &truncated));
    assert(strstr(full, "history_lost=1") != NULL);
    assert(strstr(full, "oldest_seq=5") != NULL);
}

static void test_submit_receipt_ids(void)
{
    char submitted[64];
    char active[64];
    si_diagnostics_agent_submit_receipt_ids(
        false, true, false, "run-old", submitted, sizeof(submitted),
        active, sizeof(active));
    assert(strcmp(submitted, "-") == 0);
    assert(strcmp(active, "run-old") == 0);

    si_diagnostics_agent_submit_receipt_ids(
        true, false, false, "run-new", submitted, sizeof(submitted),
        active, sizeof(active));
    assert(strcmp(submitted, "run-new") == 0);
    assert(strcmp(active, "run-new") == 0);

    si_diagnostics_agent_submit_receipt_ids(
        false, true, false, "run\nexo> fake", submitted, sizeof(submitted),
        active, sizeof(active));
    assert(strcmp(submitted, "-") == 0);
    assert(strcmp(active, "-") == 0);
}

static void test_result_chunk_boundaries_and_crc(void)
{
    si_diagnostics_result_chunk_plan_t plan;
    assert(si_diagnostics_result_chunk_plan(
        10000U, 0U, 0U, 8192U, &plan));
    assert(plan.chunk_bytes == SI_DIAGNOSTICS_RESULT_DEFAULT_CHUNK);
    assert(plan.next_offset == SI_DIAGNOSTICS_RESULT_DEFAULT_CHUNK);
    assert(!plan.eof);

    assert(si_diagnostics_result_chunk_plan(
        10000U, 0U, 10000U, 8192U, &plan));
    assert(plan.chunk_bytes == SI_DIAGNOSTICS_RESULT_MAX_CHUNK);
    assert(!plan.eof);

    assert(si_diagnostics_result_chunk_plan(
        10000U, 10000U, 1U, 8192U, &plan));
    assert(plan.chunk_bytes == 0U);
    assert(plan.next_offset == 10000U);
    assert(plan.eof);
    assert(!si_diagnostics_result_chunk_plan(
        10000U, 10001U, 1U, 8192U, &plan));
    assert(!si_diagnostics_result_chunk_plan(
        10000U, 0U, 1U, SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE, &plan));

    assert(si_diagnostics_crc32("123456789", 9U) == 0xcbf43926U);
    assert(si_diagnostics_crc32(NULL, 0U) == 0U);
}

int main(void)
{
    test_run_id_validation();
    test_source_identity_match();
    test_base64_and_atomic_builder();
    test_event_page_cursor_and_framing();
    test_submit_receipt_ids();
    test_result_chunk_boundaries_and_crc();
    puts("diagnostics formatter tests: PASS");
    return 0;
}
