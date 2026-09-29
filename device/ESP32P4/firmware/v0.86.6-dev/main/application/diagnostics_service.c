#include "diagnostics_service.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static si_diagnostics_backend_t s_backend;

bool si_diagnostics_run_id_valid(const char *run_id)
{
    if (!run_id || !run_id[0]) {
        return false;
    }
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char *)run_id; *p; ++p) {
        bool allowed = (*p >= 'a' && *p <= 'z') ||
                       (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') ||
                       *p == '-' || *p == '_' || *p == '.' || *p == ':';
        if (!allowed || ++len > SI_DIAGNOSTICS_AGENT_RUN_ID_MAX) {
            return false;
        }
    }
    return true;
}

bool si_diagnostics_agent_source_identity_matches(
    const si_diagnostics_agent_source_identity_t *observed,
    const si_diagnostics_agent_source_identity_t *required)
{
    return observed && required && observed->auth_id && required->auth_id &&
           observed->auth_id[0] && required->auth_id[0] &&
           observed->origin == required->origin &&
           observed->auth_kind == required->auth_kind &&
           observed->principal == required->principal &&
           observed->auth_generation == required->auth_generation &&
           observed->authority_ceiling == required->authority_ceiling &&
           strcmp(observed->auth_id, required->auth_id) == 0;
}

void si_diagnostics_text_builder_init(si_diagnostics_text_builder_t *builder,
                                      char *out,
                                      size_t out_size)
{
    if (!builder) {
        return;
    }
    builder->out = out;
    builder->capacity = out_size;
    builder->length = 0;
    if (out && out_size > 0) {
        out[0] = '\0';
    }
}

bool si_diagnostics_text_appendf(si_diagnostics_text_builder_t *builder,
                                 const char *fmt, ...)
{
    if (!builder || !builder->out || builder->capacity == 0 || !fmt ||
        builder->length >= builder->capacity) {
        return false;
    }
    size_t old_length = builder->length;
    size_t remaining = builder->capacity - old_length;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(builder->out + old_length, remaining, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= remaining) {
        builder->out[old_length] = '\0';
        return false;
    }
    builder->length += (size_t)written;
    return true;
}

bool si_diagnostics_base64_encode(const void *data,
                                  size_t data_len,
                                  char *out,
                                  size_t out_size,
                                  size_t *written_out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (written_out) {
        *written_out = 0;
    }
    if (!out || out_size == 0 || (!data && data_len > 0) ||
        data_len > (SIZE_MAX - 2U) / 3U) {
        return false;
    }
    size_t encoded_len = 4U * ((data_len + 2U) / 3U);
    if (encoded_len >= out_size) {
        out[0] = '\0';
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t source = 0;
    size_t dest = 0;
    while (source + 3U <= data_len) {
        uint32_t value = ((uint32_t)bytes[source] << 16U) |
                         ((uint32_t)bytes[source + 1U] << 8U) |
                         (uint32_t)bytes[source + 2U];
        out[dest++] = alphabet[(value >> 18U) & 0x3fU];
        out[dest++] = alphabet[(value >> 12U) & 0x3fU];
        out[dest++] = alphabet[(value >> 6U) & 0x3fU];
        out[dest++] = alphabet[value & 0x3fU];
        source += 3U;
    }
    size_t tail = data_len - source;
    if (tail > 0) {
        uint32_t value = (uint32_t)bytes[source] << 16U;
        if (tail == 2U) {
            value |= (uint32_t)bytes[source + 1U] << 8U;
        }
        out[dest++] = alphabet[(value >> 18U) & 0x3fU];
        out[dest++] = alphabet[(value >> 12U) & 0x3fU];
        out[dest++] = tail == 2U ? alphabet[(value >> 6U) & 0x3fU] : '=';
        out[dest++] = '=';
    }
    out[dest] = '\0';
    if (written_out) {
        *written_out = dest;
    }
    return true;
}

bool si_diagnostics_text_append_base64_line(
    si_diagnostics_text_builder_t *builder,
    const char *trusted_prefix,
    const void *data,
    size_t data_len)
{
    if (!builder || !builder->out || !trusted_prefix ||
        (!data && data_len > 0) || data_len > (SIZE_MAX - 2U) / 3U) {
        return false;
    }
    size_t prefix_len = strlen(trusted_prefix);
    size_t encoded_len = 4U * ((data_len + 2U) / 3U);
    if (builder->length >= builder->capacity ||
        prefix_len > SIZE_MAX - encoded_len - 2U ||
        prefix_len + encoded_len + 2U >
            builder->capacity - builder->length) {
        return false;
    }
    size_t old_length = builder->length;
    memcpy(builder->out + old_length, trusted_prefix, prefix_len);
    size_t written = 0;
    if (!si_diagnostics_base64_encode(
            data, data_len, builder->out + old_length + prefix_len,
            builder->capacity - old_length - prefix_len, &written)) {
        builder->out[old_length] = '\0';
        return false;
    }
    builder->length = old_length + prefix_len + written;
    builder->out[builder->length++] = '\n';
    builder->out[builder->length] = '\0';
    return true;
}

bool si_diagnostics_format_agent_event_page(
    const si_diagnostics_agent_event_page_t *page,
    char *out,
    size_t out_size,
    uint32_t *next_after_seq_out,
    bool *truncated_out)
{
    if (next_after_seq_out) {
        *next_after_seq_out = page ? page->after_seq : 0U;
    }
    if (truncated_out) {
        *truncated_out = false;
    }
    if (!page || !out || out_size == 0 ||
        !si_diagnostics_run_id_valid(page->run_id) ||
        (page->event_count > 0 && !page->events)) {
        if (out && out_size > 0) {
            snprintf(out, out_size, "agent-events error=invalid_argument\n");
        }
        return false;
    }

    char trailer_probe[320];
    int trailer_probe_len = snprintf(
        trailer_probe, sizeof(trailer_probe),
        "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
        " available_latest_seq=%" PRIu32
        " truncated=1 history_lost=1 oldest_seq=%" PRIu32
        " dropped=%" PRIu32 " running=1\n",
        page->run_id, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
    if (trailer_probe_len < 0 ||
        (size_t)trailer_probe_len >= sizeof(trailer_probe) ||
        (size_t)trailer_probe_len + 1U > out_size) {
        snprintf(out, out_size, "agent-events error=output_too_small\n");
        return false;
    }

    si_diagnostics_text_builder_t builder;
    si_diagnostics_text_builder_init(&builder, out, out_size);
    uint32_t next_after_seq = page->after_seq;
    bool truncated = false;
    for (size_t i = 0; i < page->event_count; ++i) {
        const si_action_event_t *event = &page->events[i];
        if (event->seq <= page->after_seq) {
            continue;
        }
        char action_id_b64[4U * ((SI_ACTION_EVENT_ACTION_ID_MAX + 2U) / 3U) + 1U];
        char summary_b64[4U * ((SI_ACTION_EVENT_SUMMARY_MAX + 2U) / 3U) + 1U];
        if (!si_diagnostics_base64_encode(
                event->action_id, strlen(event->action_id),
                action_id_b64, sizeof(action_id_b64), NULL) ||
            !si_diagnostics_base64_encode(
                event->summary, strlen(event->summary),
                summary_b64, sizeof(summary_b64), NULL)) {
            truncated = true;
            break;
        }
        char line[768];
        int line_len = snprintf(
            line, sizeof(line),
            "event seq=%" PRIu32 " ms=%" PRIu32
            " actor=%s channel=%s phase=%s kind=%s redacted=%d background=%d"
            " action_id_b64=%s summary_b64=%s\n",
            event->seq, event->timestamp_ms,
            si_action_actor_name(event->actor),
            si_action_channel_name(event->channel),
            si_action_phase_name(event->phase),
            si_action_phase_legacy_kind(event->phase),
            event->redacted, event->background,
            action_id_b64, summary_b64);
        if (line_len < 0 || (size_t)line_len >= sizeof(line) ||
            builder.length + (size_t)line_len +
                    (size_t)trailer_probe_len + 1U >
                builder.capacity ||
            !si_diagnostics_text_appendf(&builder, "%s", line)) {
            truncated = true;
            break;
        }
        next_after_seq = event->seq;
    }
    bool trailer_ok = si_diagnostics_text_appendf(
        &builder,
        "agent-events-trailer run_id=%s next_after_seq=%" PRIu32
        " available_latest_seq=%" PRIu32
        " truncated=%d history_lost=%d oldest_seq=%" PRIu32
        " dropped=%" PRIu32 " running=%d\n",
        page->run_id, next_after_seq, page->available_latest_seq,
        truncated, page->history_lost, page->oldest_seq,
        page->dropped, page->running);
    if (next_after_seq_out) {
        *next_after_seq_out = next_after_seq;
    }
    if (truncated_out) {
        *truncated_out = truncated;
    }
    return trailer_ok;
}

void si_diagnostics_agent_submit_receipt_ids(
    bool accepted,
    bool busy,
    bool deduplicated,
    const char *observed_run_id,
    char *submitted_run_id,
    size_t submitted_run_id_size,
    char *active_run_id,
    size_t active_run_id_size)
{
    const char *valid_run = si_diagnostics_run_id_valid(observed_run_id) ?
        observed_run_id : "-";
    const char *submitted = (accepted || deduplicated) ? valid_run : "-";
    const char *active = (accepted || busy || deduplicated) ? valid_run : "-";
    if (submitted_run_id && submitted_run_id_size > 0) {
        snprintf(submitted_run_id, submitted_run_id_size, "%s", submitted);
    }
    if (active_run_id && active_run_id_size > 0) {
        snprintf(active_run_id, active_run_id_size, "%s", active);
    }
}

bool si_diagnostics_result_chunk_plan(
    size_t total_bytes,
    size_t offset,
    size_t requested_bytes,
    size_t output_size,
    si_diagnostics_result_chunk_plan_t *plan)
{
    if (!plan || offset > total_bytes ||
        output_size <= SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE) {
        return false;
    }
    size_t requested = requested_bytes > 0 ? requested_bytes :
        SI_DIAGNOSTICS_RESULT_DEFAULT_CHUNK;
    if (requested > SI_DIAGNOSTICS_RESULT_MAX_CHUNK) {
        requested = SI_DIAGNOSTICS_RESULT_MAX_CHUNK;
    }
    size_t encoded_capacity =
        output_size - SI_DIAGNOSTICS_RESULT_FRAMING_RESERVE;
    size_t fit_raw = (encoded_capacity / 4U) * 3U;
    if (requested > fit_raw) {
        requested = fit_raw;
    }
    size_t remaining = total_bytes - offset;
    if (remaining > 0 && requested == 0) {
        return false;
    }
    size_t chunk_bytes = remaining < requested ? remaining : requested;
    plan->offset = offset;
    plan->chunk_bytes = chunk_bytes;
    plan->next_offset = offset + chunk_bytes;
    plan->eof = plan->next_offset >= total_bytes;
    return true;
}

uint32_t si_diagnostics_crc32(const void *data, size_t data_len)
{
    if (!data && data_len > 0) {
        return 0U;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < data_len; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

static void unavailable(char *out, size_t out_size, const char *operation)
{
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s unavailable\n", operation);
}

void si_diagnostics_register_backend(const si_diagnostics_backend_t *backend)
{
    if (backend) {
        s_backend = *backend;
    } else {
        memset(&s_backend, 0, sizeof(s_backend));
    }
}

int si_diagnostics_web_client_count(void)
{
    return s_backend.web_client_count ? s_backend.web_client_count() : 0;
}

#define FORWARD_OUTPUT(member, operation)                                      \
    do {                                                                        \
        if (s_backend.member) {                                                  \
            s_backend.member(out, out_size);                                    \
        } else {                                                                \
            unavailable(out, out_size, operation);                              \
        }                                                                       \
    } while (0)

void si_diagnostics_agent_history(char *out, size_t out_size)
{
    FORWARD_OUTPUT(agent_history, "agent history");
}

void si_diagnostics_agent_history_append(const char *text, char *out, size_t out_size)
{
    if (s_backend.agent_history_append) {
        s_backend.agent_history_append(text, out, out_size);
    } else {
        unavailable(out, out_size, "agent history append");
    }
}

void si_diagnostics_agent_history_clear(char *out, size_t out_size)
{
    FORWARD_OUTPUT(agent_history_clear, "agent history clear");
}

void si_diagnostics_agent_memory(char *out, size_t out_size)
{
    FORWARD_OUTPUT(agent_memory, "agent memory");
}

void si_diagnostics_agent_memory_append(const char *text, char *out, size_t out_size)
{
    if (s_backend.agent_memory_append) {
        s_backend.agent_memory_append(text, out, out_size);
    } else {
        unavailable(out, out_size, "agent memory append");
    }
}

void si_diagnostics_agent_memory_clear(char *out, size_t out_size)
{
    FORWARD_OUTPUT(agent_memory_clear, "agent memory clear");
}

void si_diagnostics_agent_data_clear(char *out, size_t out_size)
{
    FORWARD_OUTPUT(agent_data_clear, "all Agent data clear");
}

void si_diagnostics_ssh_target(char *out, size_t out_size)
{
    FORWARD_OUTPUT(ssh_target, "ssh target diagnostics");
}

void si_diagnostics_ssh_exec(const char *command, char *out, size_t out_size)
{
    if (s_backend.ssh_exec) {
        s_backend.ssh_exec(command, out, out_size);
    } else {
        unavailable(out, out_size, "ssh diagnostics");
    }
}

void si_diagnostics_agent_run_start(const char *session_id,
                                    const char *message,
                                    const char *profile,
                                    const char *model,
                                    bool dry_run,
                                    bool include_screenshot,
                                    bool allow_web_search,
                                    char *out,
                                    size_t out_size)
{
    if (s_backend.agent_run_start) {
        s_backend.agent_run_start(session_id, message, profile, model, dry_run,
                                  include_screenshot, allow_web_search, out, out_size);
    } else {
        unavailable(out, out_size, "agent run");
    }
}

static bool exact_run_id_required(const char *run_id,
                                  char *out,
                                  size_t out_size)
{
    if (si_diagnostics_run_id_valid(run_id)) {
        return true;
    }
    if (out && out_size > 0) {
        snprintf(out, out_size, "valid exact run_id required\n");
    }
    return false;
}

void si_diagnostics_agent_run_status(const char *run_id,
                                     char *out,
                                     size_t out_size)
{
    if (run_id && !si_diagnostics_run_id_valid(run_id)) {
        if (out && out_size > 0) {
            snprintf(out, out_size, "invalid run_id\n");
        }
        return;
    }
    if (s_backend.agent_run_status) {
        s_backend.agent_run_status(run_id, out, out_size);
    } else {
        unavailable(out, out_size, "agent run status");
    }
}

void si_diagnostics_agent_run_events(const char *run_id,
                                     uint32_t since_seq,
                                     uint32_t *latest_seq,
                                     bool *running,
                                     char *out,
                                     size_t out_size)
{
    if (run_id && !si_diagnostics_run_id_valid(run_id)) {
        if (latest_seq) {
            *latest_seq = since_seq;
        }
        if (running) {
            *running = false;
        }
        if (out && out_size > 0) {
            snprintf(out, out_size, "invalid run_id\n");
        }
        return;
    }
    if (s_backend.agent_run_events) {
        s_backend.agent_run_events(run_id, since_seq, latest_seq, running,
                                   out, out_size);
        return;
    }
    if (latest_seq) {
        *latest_seq = since_seq;
    }
    if (running) {
        *running = false;
    }
    unavailable(out, out_size, "agent run events");
}

void si_diagnostics_agent_run_pause(const char *run_id,
                                    char *out,
                                    size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (s_backend.agent_run_pause) {
        s_backend.agent_run_pause(run_id, out, out_size);
    } else {
        unavailable(out, out_size, "agent run pause");
    }
}

void si_diagnostics_agent_run_resume(const char *run_id,
                                     char *out,
                                     size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (s_backend.agent_run_resume) {
        s_backend.agent_run_resume(run_id, out, out_size);
    } else {
        unavailable(out, out_size, "agent run resume");
    }
}

void si_diagnostics_agent_run_cancel(const char *run_id,
                                     char *out,
                                     size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (s_backend.agent_run_cancel) {
        s_backend.agent_run_cancel(run_id, out, out_size);
    } else {
        unavailable(out, out_size, "agent run cancel");
    }
}

void si_diagnostics_agent_run_abort(const char *run_id,
                                    char *out,
                                    size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (s_backend.agent_run_abort) {
        s_backend.agent_run_abort(run_id, out, out_size);
    } else {
        unavailable(out, out_size, "agent run abort");
    }
}

void si_diagnostics_agent_run_steer(const char *run_id,
                                    const char *message,
                                    char *out,
                                    size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (!message || !message[0]) {
        if (out && out_size > 0) {
            snprintf(out, out_size, "steer message required\n");
        }
        return;
    }
    if (s_backend.agent_run_steer) {
        s_backend.agent_run_steer(run_id, message, out, out_size);
    } else {
        unavailable(out, out_size, "agent run steer");
    }
}

void si_diagnostics_agent_run_result(const char *run_id,
                                     size_t offset,
                                     size_t max_bytes,
                                     char *out,
                                     size_t out_size)
{
    if (!exact_run_id_required(run_id, out, out_size)) {
        return;
    }
    if (s_backend.agent_run_result) {
        s_backend.agent_run_result(run_id, offset, max_bytes, out, out_size);
    } else {
        unavailable(out, out_size, "agent run result");
    }
}
