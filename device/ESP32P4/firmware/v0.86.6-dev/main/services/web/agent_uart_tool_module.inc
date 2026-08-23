// Non-destructive UART observation plus supervised UART writes/baud changes.
#define AGENT_UART_MAX_READ_BYTES 2048U
#define AGENT_UART_MAX_WRITE_BYTES 1024U
#define AGENT_UART_MAX_WAIT_MS 5000U

static bool agent_run_control_point(const char *job_id);

static bool agent_uart_payload_is_sensitive(const char *data)
{
    const char *const needles[] = {
        "password=", "passwd=", "api_key=", "apikey=", "token=",
        "authorization:", "bearer ", "private key", "secret=",
    };
    return agent_text_contains_any_ci(
        data, needles, sizeof(needles) / sizeof(needles[0]));
}

static void agent_uart_add_bytes(cJSON *item, const uint8_t *data, size_t len,
                                 const si_target_uart_journal_t *journal)
{
    char *text = si_utf8_sanitize((const char *)data, len);
    size_t base64_size = 4U * ((len + 2U) / 3U) + 1U;
    unsigned char *base64 = calloc(1, base64_size);
    size_t base64_len = 0;
    if (base64 && len > 0 &&
        mbedtls_base64_encode(base64, base64_size, &base64_len,
                              data, len) != 0) {
        base64[0] = '\0';
    }
    cJSON_AddNumberToObject(item, "bytes", len);
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddStringToObject(item, "base64",
                            base64 ? (const char *)base64 : "");
    cJSON_AddNumberToObject(item, "start_seq",
                            journal ? (double)journal->start_seq : 0);
    cJSON_AddNumberToObject(
        item, "next_cursor",
        journal ? (double)(journal->start_seq + len) : 0);
    cJSON_AddNumberToObject(item, "journal_next_seq",
                            journal ? (double)journal->next_seq : 0);
    cJSON_AddNumberToObject(
        item, "pending_bytes",
        journal && journal->next_seq > journal->start_seq + len ?
        (double)(journal->next_seq - journal->start_seq - len) : 0);
    cJSON_AddBoolToObject(item, "history_lost",
                          journal ? journal->history_lost : false);
    free(text);
    free(base64);
}

static size_t agent_uart_read_from_cursor(
    const char *job_id, uint64_t cursor, uint32_t wait_ms,
    uint8_t *data, size_t max_bytes, si_target_uart_journal_t *journal,
    const si_terminal_control_token_t *control)
{
    int64_t deadline = si_monotonic_ms() + wait_ms;
    while (wait_ms > 0) {
        si_target_uart_status_t status;
        si_target_uart_get_status(&status);
        if (!status.initialized ||
            cursor == 0 ||
            status.journal_next_seq > cursor ||
            si_monotonic_ms() >= deadline) {
            break;
        }
        if (!agent_run_control_point(job_id) ||
            (control && terminal_control_cancel_requested(control))) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return si_target_uart_journal_read_from(
        cursor, data, max_bytes, journal);
}

static bool agent_execute_uart_status_tool(const char *job_id,
                                           const cJSON *call, int index,
                                           bool dry_run, cJSON *results,
                                           bool *all_ok)
{
    (void)call;
    cJSON *item =
        agent_tool_result_begin(results, index, "uart_status", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    cJSON_AddBoolToObject(item, "ok", status.initialized);
    uart_add_status_json(item);
    if (!status.initialized) {
        cJSON_AddStringToObject(
            item, "error",
            status.last_error[0] ? status.last_error : "target UART unavailable");
        if (all_ok) *all_ok = false;
    }
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_UART,
        status.initialized ? SI_ACTION_PHASE_SUCCEEDED :
                             SI_ACTION_PHASE_FAILED,
        false, true, "tool[%d] uart_status · UART%d %d 8N1 · %s",
        index + 1, status.port, status.baud_rate,
        status.initialized ? "ready" : "unavailable");
    return true;
}

static bool agent_execute_uart_read_tool(const char *job_id,
                                         const cJSON *call, int index,
                                         bool dry_run, cJSON *results,
                                         bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const cJSON *cursor_item = json_number_any(args, "cursor", "after_seq");
    const cJSON *max_item = json_number_any(args, "max_bytes", "limit");
    const cJSON *wait_item = json_number_any(args, "wait_ms", "timeout_ms");
    uint64_t cursor =
        cursor_item && cursor_item->valuedouble > 0 ?
        (uint64_t)cursor_item->valuedouble : 0;
    size_t max_bytes =
        max_item && max_item->valueint > 0 ?
        (size_t)max_item->valueint : 1024U;
    if (max_bytes > AGENT_UART_MAX_READ_BYTES) {
        max_bytes = AGENT_UART_MAX_READ_BYTES;
    }
    uint32_t wait_ms =
        wait_item && wait_item->valueint > 0 ?
        (uint32_t)wait_item->valueint : 0;
    if (wait_ms > AGENT_UART_MAX_WAIT_MS) {
        wait_ms = AGENT_UART_MAX_WAIT_MS;
    }
    cJSON *item =
        agent_tool_result_begin(results, index, "uart_read", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    if (!status.initialized) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            status.last_error[0] ? status.last_error : "target UART unavailable");
        if (all_ok) *all_ok = false;
        return false;
    }
    uint8_t *data = agent_alloc(max_bytes + 1U);
    if (!data) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART read allocation failed");
        if (all_ok) *all_ok = false;
        return false;
    }
    si_target_uart_journal_t journal = {0};
    si_terminal_control_token_t control = {0};
    if (!terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_AGENT, "uart_read",
            job_id, "Agent 正在读取 UART", &control)) {
        free(data);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART automation already active");
        if (all_ok) *all_ok = false;
        return false;
    }
    size_t len = agent_uart_read_from_cursor(
        job_id, cursor, wait_ms, data, max_bytes, &journal, &control);
    (void)terminal_control_finish(&control);
    data[len] = '\0';
    cJSON_AddBoolToObject(item, "ok", true);
    char boot_id[24];
    snprintf(boot_id, sizeof(boot_id), "%016" PRIx64, journal.boot_id);
    cJSON_AddStringToObject(item, "boot_id", boot_id);
    cJSON_AddNumberToObject(item, "generation", (double)journal.generation);
    agent_uart_add_bytes(item, data, len, &journal);
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_UART, SI_ACTION_PHASE_SUCCEEDED,
        false, true, "tool[%d] uart_read · %u bytes · cursor=%" PRIu64,
        index + 1, (unsigned)len, journal.start_seq + len);
    free(data);
    return true;
}

static bool agent_execute_uart_write_tool(const char *job_id,
                                          const cJSON *call, int index,
                                          bool dry_run, cJSON *results,
                                          bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *data = json_string_any(args, "data", "text", "input");
    const cJSON *append_enter_item =
        cJSON_GetObjectItemCaseSensitive(args, "append_enter");
    if (!cJSON_IsBool(append_enter_item)) {
        append_enter_item =
            cJSON_GetObjectItemCaseSensitive(args, "append_newline");
    }
    bool append_enter = cJSON_IsTrue(append_enter_item);
    const cJSON *wait_item = json_number_any(args, "wait_ms", "timeout_ms");
    const cJSON *max_item =
        json_number_any(args, "read_max_bytes", "max_bytes");
    uint32_t wait_ms =
        wait_item && wait_item->valueint >= 0 ?
        (uint32_t)wait_item->valueint : 300U;
    if (wait_ms > AGENT_UART_MAX_WAIT_MS) {
        wait_ms = AGENT_UART_MAX_WAIT_MS;
    }
    size_t read_max =
        max_item && max_item->valueint > 0 ?
        (size_t)max_item->valueint : 1024U;
    if (read_max > AGENT_UART_MAX_READ_BYTES) {
        read_max = AGENT_UART_MAX_READ_BYTES;
    }
    cJSON *item =
        agent_tool_result_begin(results, index, "uart_write", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    size_t data_len = data ? strlen(data) : 0;
    if (!data || data_len == 0 ||
        data_len + (append_enter ? 1U : 0U) > AGENT_UART_MAX_WRITE_BYTES) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            "uart_write requires 1..1024 bytes including optional enter");
        if (all_ok) *all_ok = false;
        return false;
    }
    bool redacted = agent_uart_payload_is_sensitive(data);
    cJSON_AddBoolToObject(item, "append_enter", append_enter);
    cJSON_AddNumberToObject(item, "wait_ms", wait_ms);
    cJSON_AddBoolToObject(item, "data_redacted", redacted);
    if (!redacted) {
        cJSON_AddStringToObject(item, "data", data);
    }
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U] = {0};
    (void)agent_run_next_action_id(job_id, "uart", action_id,
                                   sizeof(action_id));
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_UART,
            SI_ACTION_PHASE_PLANNED, redacted, true,
            redacted ? "tool[%d] uart_write dry-run · data redacted" :
                       "tool[%d] uart_write dry-run · %s%s",
            index + 1, data, append_enter ? " + ENTER" : "");
        return false;
    }
    si_target_uart_status_t before;
    si_target_uart_get_status(&before);
    if (!before.initialized) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error", "target UART unavailable");
        if (all_ok) *all_ok = false;
        return false;
    }

    uint8_t payload[AGENT_UART_MAX_WRITE_BYTES];
    memcpy(payload, data, data_len);
    size_t payload_len = data_len;
    if (append_enter) {
        payload[payload_len++] = '\r';
    }
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_UART,
        SI_ACTION_PHASE_INPUT_SENT, redacted, true,
        redacted ? "tool[%d] uart_write · data redacted" :
                   "tool[%d] uart_write · %s%s",
        index + 1, data, append_enter ? " + ENTER" : "");
    si_terminal_control_token_t control = {0};
    if (!terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_AGENT, action_id,
            job_id, "Agent 向 UART 发送输入", &control)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART automation already active");
        if (all_ok) *all_ok = false;
        return false;
    }
    terminal_control_publish_input(&control, data, append_enter, redacted);
    esp_err_t ret = uart_mutation_begin();
    if (ret != ESP_OK) {
        (void)terminal_control_finish(&control);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART operation lock failed");
        if (all_ok) *all_ok = false;
        return false;
    }
    size_t written = 0;
    ret = si_target_uart_write(payload, payload_len, &written);
    uart_mutation_end();
    cJSON_AddNumberToObject(item, "bytes_written", written);
    if (ret != ESP_OK) {
        (void)terminal_control_finish(&control);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_UART,
            SI_ACTION_PHASE_FAILED, false, true,
            "tool[%d] uart_write failed · %s",
            index + 1, esp_err_to_name(ret));
        if (all_ok) *all_ok = false;
        return true;
    }

    uint8_t *response = agent_alloc(read_max + 1U);
    si_target_uart_journal_t journal = {0};
    size_t response_len = 0;
    if (response) {
        response_len = agent_uart_read_from_cursor(
            job_id, before.journal_next_seq, wait_ms,
            response, read_max, &journal, &control);
        response[response_len] = '\0';
    }
    cJSON_AddBoolToObject(item, "ok", true);
    cJSON_AddNumberToObject(item, "cursor_before",
                            (double)before.journal_next_seq);
    if (response) {
        cJSON *output = cJSON_AddObjectToObject(item, "output");
        agent_uart_add_bytes(output, response, response_len, &journal);
        free(response);
    } else {
        cJSON_AddStringToObject(item, "output_error",
                                "UART response allocation failed");
    }
    (void)terminal_control_finish(&control);
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_UART,
        SI_ACTION_PHASE_SUCCEEDED, false, true,
        "tool[%d] uart_write ok · wrote=%u response=%u",
        index + 1, (unsigned)written, (unsigned)response_len);
    return true;
}

static bool agent_execute_uart_auth_tool(const char *job_id,
                                         const cJSON *call, int index,
                                         bool dry_run, cJSON *results,
                                         bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const char *credential_ref = json_string_any(
        args, "credential_ref", "credential", NULL);
    const char *expected_prompt = json_string_any(
        args, "expected_prompt", "prompt", NULL);
    const cJSON *wait_item = json_number_any(args, "wait_ms", "timeout_ms");
    const cJSON *max_item =
        json_number_any(args, "read_max_bytes", "max_bytes");
    if (!credential_ref) credential_ref = "auto://sudo";
    uint32_t wait_ms = wait_item && wait_item->valueint >= 0 ?
        (uint32_t)wait_item->valueint : 500U;
    if (wait_ms > AGENT_UART_MAX_WAIT_MS) wait_ms = AGENT_UART_MAX_WAIT_MS;
    size_t read_max = max_item && max_item->valueint > 0 ?
        (size_t)max_item->valueint : 1024U;
    if (read_max > AGENT_UART_MAX_READ_BYTES)
        read_max = AGENT_UART_MAX_READ_BYTES;
    cJSON *item =
        agent_tool_result_begin(results, index, "uart_auth", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    cJSON_AddStringToObject(item, "credential_ref", credential_ref);
    cJSON_AddBoolToObject(item, "credential_redacted", true);
    cJSON_AddNumberToObject(item, "wait_ms", wait_ms);
    if (!uart_auth_credential_ref_valid(credential_ref) ||
        !uart_auth_prompt_valid(expected_prompt)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(
            item, "error",
            "uart_auth requires a supported credential_ref and an exact printable password expected_prompt");
        if (all_ok) *all_ok = false;
        return false;
    }
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        return false;
    }
    char action_id[SI_ACTION_EVENT_ACTION_ID_MAX + 1U] = {0};
    (void)agent_run_next_action_id(job_id, "uart-auth", action_id,
                                   sizeof(action_id));
    uint8_t *output = agent_alloc(read_max + 1U);
    if (!output) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "UART response allocation failed");
        if (all_ok) *all_ok = false;
        return false;
    }
    uart_auth_result_t auth = {0};
    char error[128] = {0};
    agent_run_add_action_eventf(
        job_id, action_id, SI_ACTION_CHANNEL_UART,
        SI_ACTION_PHASE_STARTED, true, true,
        "tool[%d] uart_auth · local credential submission",
        index + 1);
    esp_err_t ret = uart_authenticate_local(
        SI_TERMINAL_ACTOR_AGENT, job_id, action_id, credential_ref,
        expected_prompt, wait_ms, output, read_max, &auth,
        error, sizeof(error));
    cJSON_AddBoolToObject(item, "ok", ret == ESP_OK);
    cJSON_AddBoolToObject(item, "submitted", ret == ESP_OK);
    cJSON_AddNumberToObject(item, "cursor_before",
                            (double)auth.cursor_before);
    if (auth.credential_source[0])
        cJSON_AddStringToObject(item, "credential_source",
                                auth.credential_source);
    if (ret == ESP_OK) {
        cJSON *response = cJSON_AddObjectToObject(item, "output");
        agent_uart_add_bytes(response, output, auth.output_bytes,
                             &auth.journal);
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_UART,
            SI_ACTION_PHASE_SUCCEEDED, true, true,
            "tool[%d] uart_auth ok · credential remained device-local",
            index + 1);
    } else {
        cJSON_AddStringToObject(item, "error",
                                error[0] ? error : esp_err_to_name(ret));
        agent_run_add_action_eventf(
            job_id, action_id, SI_ACTION_CHANNEL_UART,
            SI_ACTION_PHASE_FAILED, true, true,
            "tool[%d] uart_auth failed · %s", index + 1,
            error[0] ? error : esp_err_to_name(ret));
        if (all_ok) *all_ok = false;
    }
    free(output);
    return true;
}

static bool agent_execute_uart_baud_tool(const char *job_id,
                                         const cJSON *call, int index,
                                         bool dry_run, cJSON *results,
                                         bool *all_ok)
{
    const cJSON *args = agent_tool_args(call);
    const cJSON *baud_item = json_number_any(args, "baud_rate", "baud");
    int baud_rate = baud_item ? baud_item->valueint : 0;
    cJSON *item =
        agent_tool_result_begin(results, index, "uart_baud", dry_run);
    if (!item) {
        if (all_ok) *all_ok = false;
        return false;
    }
    si_target_uart_status_t status;
    si_target_uart_get_status(&status);
    if (baud_rate != status.default_baud_rate &&
        baud_rate != status.fallback_baud_rate) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error",
                                "unsupported UART baud mode");
        if (all_ok) *all_ok = false;
        return false;
    }
    cJSON_AddNumberToObject(item, "baud_rate", baud_rate);
    if (dry_run) {
        cJSON_AddBoolToObject(item, "ok", true);
        cJSON_AddStringToObject(item, "status", "dry-run");
        return false;
    }
    si_terminal_control_token_t control = {0};
    if (!terminal_control_begin(
            SI_TERMINAL_CHANNEL_UART, SI_TERMINAL_ACTOR_AGENT, "uart_baud",
            job_id, "Agent 切换 UART 波特率", &control)) {
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART automation already active");
        if (all_ok) *all_ok = false;
        return false;
    }
    esp_err_t ret = uart_mutation_begin();
    if (ret != ESP_OK) {
        (void)terminal_control_finish(&control);
        cJSON_AddBoolToObject(item, "ok", false);
        cJSON_AddStringToObject(item, "error", "UART operation lock failed");
        if (all_ok) *all_ok = false;
        return false;
    }
    ret = si_target_uart_set_baud_rate(baud_rate);
    uart_mutation_end();
    (void)terminal_control_finish(&control);
    bool ok = ret == ESP_OK;
    cJSON_AddBoolToObject(item, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(item, "error", esp_err_to_name(ret));
        if (all_ok) *all_ok = false;
    }
    uart_add_status_json(item);
    agent_run_add_action_eventf(
        job_id, NULL, SI_ACTION_CHANNEL_UART,
        ok ? SI_ACTION_PHASE_SUCCEEDED : SI_ACTION_PHASE_FAILED,
        false, true, "tool[%d] uart_baud %s · %d",
        index + 1, ok ? "ok" : "failed", baud_rate);
    return true;
}
