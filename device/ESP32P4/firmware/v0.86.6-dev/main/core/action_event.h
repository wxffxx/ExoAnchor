#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SI_ACTION_EVENT_SCHEMA_VERSION 1U
#define SI_ACTION_EVENT_CAPACITY 64U
#define SI_ACTION_EVENT_RUN_ID_MAX 32U
#define SI_ACTION_EVENT_ACTION_ID_MAX 32U
#define SI_ACTION_EVENT_SUMMARY_MAX 240U

typedef enum {
    SI_ACTION_ACTOR_SYSTEM = 0,
    SI_ACTION_ACTOR_USER,
    SI_ACTION_ACTOR_AGENT,
    SI_ACTION_ACTOR_MCP,
} si_action_actor_t;

typedef enum {
    SI_ACTION_CHANNEL_SYSTEM = 0,
    SI_ACTION_CHANNEL_AGENT,
    SI_ACTION_CHANNEL_HID,
    SI_ACTION_CHANNEL_SSH,
    SI_ACTION_CHANNEL_UART,
    SI_ACTION_CHANNEL_VIDEO,
    SI_ACTION_CHANNEL_OTA,
} si_action_channel_t;

typedef enum {
    SI_ACTION_PHASE_PLANNED = 0,
    SI_ACTION_PHASE_WAITING_APPROVAL,
    SI_ACTION_PHASE_STARTED,
    SI_ACTION_PHASE_INPUT_SENT,
    SI_ACTION_PHASE_OUTPUT_RECEIVED,
    SI_ACTION_PHASE_VERIFYING,
    SI_ACTION_PHASE_SUCCEEDED,
    SI_ACTION_PHASE_FAILED,
    SI_ACTION_PHASE_CANCELLED,
} si_action_phase_t;

typedef struct {
    const char *run_id;
    const char *action_id;
    si_action_actor_t actor;
    si_action_channel_t channel;
    si_action_phase_t phase;
    const char *summary;
    bool redacted;
    bool background;
} si_action_event_input_t;

typedef struct {
    uint32_t schema_version;
    uint32_t seq;
    uint32_t timestamp_ms;
    char run_id[SI_ACTION_EVENT_RUN_ID_MAX + 1U];
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U];
    si_action_actor_t actor;
    si_action_channel_t channel;
    si_action_phase_t phase;
    char summary[SI_ACTION_EVENT_SUMMARY_MAX + 1U];
    bool redacted;
    bool background;
} si_action_event_t;

typedef struct {
    si_action_event_t events[SI_ACTION_EVENT_CAPACITY];
    uint32_t latest_seq;
    uint32_t count;
    uint32_t write_pos;
    uint32_t dropped;
} si_action_event_buffer_t;

void si_action_event_buffer_init(si_action_event_buffer_t *buffer);

bool si_action_event_buffer_append(si_action_event_buffer_t *buffer,
                                   uint32_t timestamp_ms,
                                   const si_action_event_input_t *input,
                                   si_action_event_t *event_out);

size_t si_action_event_buffer_read_after(const si_action_event_buffer_t *buffer,
                                         uint32_t after_seq,
                                         si_action_event_t *events_out,
                                         size_t events_capacity,
                                         uint32_t *latest_seq_out,
                                         bool *history_lost_out);

const char *si_action_actor_name(si_action_actor_t actor);
const char *si_action_channel_name(si_action_channel_t channel);
const char *si_action_phase_name(si_action_phase_t phase);
const char *si_action_phase_legacy_kind(si_action_phase_t phase);
