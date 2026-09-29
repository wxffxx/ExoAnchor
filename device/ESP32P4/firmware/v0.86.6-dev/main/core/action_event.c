#include "action_event.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value ? value : "");
}

static bool actor_valid(si_action_actor_t actor)
{
    return actor >= SI_ACTION_ACTOR_SYSTEM && actor <= SI_ACTION_ACTOR_MCP;
}

static bool channel_valid(si_action_channel_t channel)
{
    return channel >= SI_ACTION_CHANNEL_SYSTEM && channel <= SI_ACTION_CHANNEL_OTA;
}

static bool phase_valid(si_action_phase_t phase)
{
    return phase >= SI_ACTION_PHASE_PLANNED && phase <= SI_ACTION_PHASE_CANCELLED;
}

void si_action_event_buffer_init(si_action_event_buffer_t *buffer)
{
    if (buffer) {
        memset(buffer, 0, sizeof(*buffer));
    }
}

bool si_action_event_buffer_append(si_action_event_buffer_t *buffer,
                                   uint32_t timestamp_ms,
                                   const si_action_event_input_t *input,
                                   si_action_event_t *event_out)
{
    if (!buffer || !input || !actor_valid(input->actor) ||
        !channel_valid(input->channel) || !phase_valid(input->phase)) {
        return false;
    }

    uint32_t next_seq = buffer->latest_seq + 1U;
    if (next_seq == 0U) {
        next_seq = 1U;
    }
    si_action_event_t *event =
        &buffer->events[buffer->write_pos % SI_ACTION_EVENT_CAPACITY];
    memset(event, 0, sizeof(*event));
    event->schema_version = SI_ACTION_EVENT_SCHEMA_VERSION;
    event->seq = next_seq;
    event->timestamp_ms = timestamp_ms;
    event->actor = input->actor;
    event->channel = input->channel;
    event->phase = input->phase;
    event->redacted = input->redacted;
    event->background = input->background;
    copy_text(event->run_id, sizeof(event->run_id), input->run_id);
    if (input->action_id && input->action_id[0]) {
        copy_text(event->action_id, sizeof(event->action_id), input->action_id);
    } else {
        snprintf(event->action_id, sizeof(event->action_id), "a%lu",
                 (unsigned long)next_seq);
    }
    copy_text(event->summary, sizeof(event->summary), input->summary);

    buffer->latest_seq = next_seq;
    buffer->write_pos = (buffer->write_pos + 1U) % SI_ACTION_EVENT_CAPACITY;
    if (buffer->count < SI_ACTION_EVENT_CAPACITY) {
        buffer->count++;
    } else {
        buffer->dropped++;
    }
    if (event_out) {
        *event_out = *event;
    }
    return true;
}

size_t si_action_event_buffer_read_after(const si_action_event_buffer_t *buffer,
                                         uint32_t after_seq,
                                         si_action_event_t *events_out,
                                         size_t events_capacity,
                                         uint32_t *latest_seq_out,
                                         bool *history_lost_out)
{
    if (latest_seq_out) {
        *latest_seq_out = buffer ? buffer->latest_seq : after_seq;
    }
    if (history_lost_out) {
        *history_lost_out = false;
    }
    if (!buffer || !events_out || events_capacity == 0U || buffer->count == 0U) {
        return 0U;
    }

    uint32_t start =
        (buffer->write_pos + SI_ACTION_EVENT_CAPACITY - buffer->count) %
        SI_ACTION_EVENT_CAPACITY;
    const si_action_event_t *oldest = &buffer->events[start];
    if (history_lost_out && after_seq != 0U &&
        oldest->seq > 1U && after_seq < oldest->seq - 1U) {
        *history_lost_out = true;
    }

    size_t copied = 0U;
    for (uint32_t i = 0; i < buffer->count && copied < events_capacity; i++) {
        const si_action_event_t *event =
            &buffer->events[(start + i) % SI_ACTION_EVENT_CAPACITY];
        if (event->seq <= after_seq) {
            continue;
        }
        events_out[copied++] = *event;
    }
    return copied;
}

const char *si_action_actor_name(si_action_actor_t actor)
{
    switch (actor) {
    case SI_ACTION_ACTOR_USER:
        return "user";
    case SI_ACTION_ACTOR_AGENT:
        return "agent";
    case SI_ACTION_ACTOR_MCP:
        return "mcp";
    case SI_ACTION_ACTOR_SYSTEM:
    default:
        return "system";
    }
}

const char *si_action_channel_name(si_action_channel_t channel)
{
    switch (channel) {
    case SI_ACTION_CHANNEL_AGENT:
        return "agent";
    case SI_ACTION_CHANNEL_HID:
        return "hid";
    case SI_ACTION_CHANNEL_SSH:
        return "ssh";
    case SI_ACTION_CHANNEL_UART:
        return "uart";
    case SI_ACTION_CHANNEL_VIDEO:
        return "video";
    case SI_ACTION_CHANNEL_OTA:
        return "ota";
    case SI_ACTION_CHANNEL_SYSTEM:
    default:
        return "system";
    }
}

const char *si_action_phase_name(si_action_phase_t phase)
{
    switch (phase) {
    case SI_ACTION_PHASE_WAITING_APPROVAL:
        return "waiting_approval";
    case SI_ACTION_PHASE_STARTED:
        return "started";
    case SI_ACTION_PHASE_INPUT_SENT:
        return "input_sent";
    case SI_ACTION_PHASE_OUTPUT_RECEIVED:
        return "output_received";
    case SI_ACTION_PHASE_VERIFYING:
        return "verifying";
    case SI_ACTION_PHASE_SUCCEEDED:
        return "succeeded";
    case SI_ACTION_PHASE_FAILED:
        return "failed";
    case SI_ACTION_PHASE_CANCELLED:
        return "cancelled";
    case SI_ACTION_PHASE_PLANNED:
    default:
        return "planned";
    }
}

const char *si_action_phase_legacy_kind(si_action_phase_t phase)
{
    switch (phase) {
    case SI_ACTION_PHASE_INPUT_SENT:
        return "cmd";
    case SI_ACTION_PHASE_OUTPUT_RECEIVED:
    case SI_ACTION_PHASE_VERIFYING:
        return "progress";
    case SI_ACTION_PHASE_SUCCEEDED:
        return "ok";
    case SI_ACTION_PHASE_FAILED:
        return "bad";
    case SI_ACTION_PHASE_CANCELLED:
        return "warn";
    case SI_ACTION_PHASE_PLANNED:
    case SI_ACTION_PHASE_WAITING_APPROVAL:
    case SI_ACTION_PHASE_STARTED:
    default:
        return "info";
    }
}
