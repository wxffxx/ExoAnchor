// Device-owned Agent Request Broker projection and execution gate.
static void agent_run_add_action_eventf(const char *job_id,
                                        const char *action_id,
                                        si_action_channel_t channel,
                                        si_action_phase_t phase,
                                        bool redacted,
                                        bool background,
                                        const char *fmt, ...);

#define AGENT_REQUEST_BRIDGE_SESSION_ID "task-service"
#define AGENT_REQUEST_BRIDGE_INSTANCE_ID "embedded-agent-runtime-v2"

typedef struct {
    char device_id[SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN + 1];
    char thread_id[SI_AGENT_REQUEST_THREAD_ID_MAX_LEN + 1];
    char turn_id[SI_AGENT_REQUEST_TURN_ID_MAX_LEN + 1];
    char run_id[SI_AGENT_REQUEST_RUN_ID_MAX_LEN + 1];
    char step_id[SI_AGENT_REQUEST_STEP_ID_MAX_LEN + 1];
    char normalized_args_json[SI_AGENT_REQUEST_PARAMS_MAX_LEN + 1];
    char idempotency_key[SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN + 1];
    uint32_t plan_version;
    si_agent_turn_status_t turn_status;
    si_agent_task_origin_t origin;
    si_agent_task_auth_kind_t source_auth_kind;
    si_principal_kind_t source_principal;
    char source_auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U];
    uint32_t source_auth_generation;
    bool live_authenticated;
    si_principal_kind_t live_principal;
    uint32_t live_auth_generation;
    si_capability_set_t live_capabilities;
    si_capability_set_t authority_ceiling;
} agent_request_bridge_binding_t;

static uint32_t s_agent_request_bridge_sequence;

static bool agent_request_current_device_identity(
    char out[SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN + 1])
{
    if (!out) {
        return false;
    }
    out[0] = '\0';
    si_device_network_observation_t network = {0};
    si_device_observation_get_network(&network);
    size_t length = strnlen(network.device_id, sizeof(network.device_id));
    if (length == 0U || length >= sizeof(network.device_id) ||
        length > SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN) {
        return false;
    }
    memcpy(out, network.device_id, length);
    out[length] = '\0';
    return true;
}

static si_capability_t agent_request_required_capability(
    const char *resource)
{
    if (!resource) return SI_CAPABILITY_AGENT_RUN;
    if (strstr(resource, "hid") || strstr(resource, "console.login") ||
        strstr(resource, "boot_key")) return SI_CAPABILITY_HID;
    if (strstr(resource, "power")) return SI_CAPABILITY_POWER;
    if (strstr(resource, "uart")) return SI_CAPABILITY_UART;
    if (strstr(resource, "ssh") || strstr(resource, "service") ||
        strstr(resource, "port")) return SI_CAPABILITY_SSH;
    if (strstr(resource, "memory") || strstr(resource, "history"))
        return SI_CAPABILITY_STORAGE;
    if (strstr(resource, "host_display") || strstr(resource, "video"))
        return SI_CAPABILITY_VIDEO_CONTROL;
    if (strstr(resource, "observe") || strstr(resource, "status") ||
        strstr(resource, "wait") || strstr(resource, "web_search"))
        return SI_CAPABILITY_OBSERVE;
    return SI_CAPABILITY_AGENT_RUN;
}

static si_authorization_risk_t agent_request_authorization_risk(
    si_agent_request_risk_t risk)
{
    switch (risk) {
    case SI_AGENT_REQUEST_RISK_LOW: return SI_AUTHZ_RISK_LOW;
    case SI_AGENT_REQUEST_RISK_MEDIUM: return SI_AUTHZ_RISK_MEDIUM;
    case SI_AGENT_REQUEST_RISK_HIGH: return SI_AUTHZ_RISK_HIGH;
    case SI_AGENT_REQUEST_RISK_CRITICAL: return SI_AUTHZ_RISK_CRITICAL;
    default: return SI_AUTHZ_RISK_CRITICAL;
    }
}

static si_authorization_mode_t agent_request_authorization_mode(void)
{
    return si_agent_access_mode_get() == SI_AGENT_ACCESS_FULL ?
           SI_AUTHZ_MODE_AUTONOMOUS : SI_AUTHZ_MODE_SUPERVISED;
}

static const char *agent_request_policy_tool_name(
    const char *resource, char out[SI_AGENT_TASK_TOOL_MAX_LEN + 1U])
{
    const char *name = resource ? resource : "unknown";
    if (strcmp(name, "console.login") == 0) name = "console_login";
    else if (strcmp(name, "uart.write") == 0) name = "uart_write";
    else if (strcmp(name, "uart.auth") == 0) name = "uart_auth";
    else if (strcmp(name, "uart.baud") == 0) name = "uart_baud";
    else if (strncmp(name, "host_display.", 13U) == 0)
        name = "host_display";
    else if (strncmp(name, "boot_key_sequence.", 18U) == 0)
        name = "boot_key_sequence";
    strlcpy(out, name, SI_AGENT_TASK_TOOL_MAX_LEN + 1U);
    return out;
}

static int agent_request_json_key_compare(const void *left,
                                          const void *right)
{
    const cJSON *left_item = *(const cJSON *const *)left;
    const cJSON *right_item = *(const cJSON *const *)right;
    const char *left_key = left_item && left_item->string ?
                           left_item->string : "";
    const char *right_key = right_item && right_item->string ?
                            right_item->string : "";
    return strcmp(left_key, right_key);
}

/* Canonicalize object key order before the Broker hashes exact arguments. */
static cJSON *agent_request_json_canonical_clone(const cJSON *item)
{
    if (!item) {
        return NULL;
    }
    if (cJSON_IsArray(item)) {
        cJSON *copy = cJSON_CreateArray();
        if (!copy) {
            return NULL;
        }
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            cJSON *child_copy = agent_request_json_canonical_clone(child);
            if (!child_copy) {
                cJSON_Delete(copy);
                return NULL;
            }
            cJSON_AddItemToArray(copy, child_copy);
        }
        return copy;
    }
    if (!cJSON_IsObject(item)) {
        return cJSON_Duplicate(item, false);
    }

    size_t count = (size_t)cJSON_GetArraySize(item);
    const cJSON **children = count > 0 ?
        agent_alloc(count * sizeof(*children)) : NULL;
    if (count > 0 && !children) {
        return NULL;
    }
    size_t index = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, item) {
        children[index++] = child;
    }
    qsort(children, count, sizeof(*children), agent_request_json_key_compare);

    cJSON *copy = cJSON_CreateObject();
    for (index = 0; copy && index < count; ++index) {
        const char *key = children[index]->string;
        if (!key || (index > 0 &&
                     strcmp(key, children[index - 1]->string) == 0)) {
            cJSON_Delete(copy);
            copy = NULL;
            break;
        }
        cJSON *child_copy =
            agent_request_json_canonical_clone(children[index]);
        if (!child_copy) {
            cJSON_Delete(copy);
            copy = NULL;
            break;
        }
        cJSON_AddItemToObject(copy, key, child_copy);
    }
    free(children);
    return copy;
}

static bool agent_request_normalize_args(
    const char *params_json,
    char out[SI_AGENT_REQUEST_PARAMS_MAX_LEN + 1],
    char *error, size_t error_size)
{
    const char *source = params_json && params_json[0] ? params_json : "{}";
    size_t source_length = 0U;
    while (source_length <= SI_AGENT_REQUEST_PARAMS_MAX_LEN &&
           source[source_length] != '\0') {
        ++source_length;
    }
    if (source_length > SI_AGENT_REQUEST_PARAMS_MAX_LEN) {
        if (error && error_size > 0) {
            strlcpy(error, "Agent request arguments exceed the size limit",
                    error_size);
        }
        return false;
    }
    cJSON *parsed = cJSON_ParseWithOpts(source, NULL, true);
    cJSON *canonical = parsed ?
        agent_request_json_canonical_clone(parsed) : NULL;
    char *printed = canonical ? cJSON_PrintUnformatted(canonical) : NULL;
    bool ok = printed && printed[0] &&
              strlen(printed) <= SI_AGENT_REQUEST_PARAMS_MAX_LEN;
    if (ok) {
        strlcpy(out, printed, SI_AGENT_REQUEST_PARAMS_MAX_LEN + 1);
    } else if (error && error_size > 0) {
        strlcpy(error,
                parsed ? "Agent request arguments are too large or ambiguous" :
                         "Agent request arguments are not valid JSON",
                error_size);
    }
    free(printed);
    cJSON_Delete(canonical);
    cJSON_Delete(parsed);
    return ok;
}

static const si_agent_task_step_t *agent_request_current_step(
    const si_agent_task_turn_t *turn)
{
    if (!turn || !turn->has_plan || turn->plan.version == 0U ||
        turn->plan.step_count > SI_AGENT_TASK_STEP_CAPACITY) {
        return NULL;
    }
    for (size_t index = 0; index < turn->plan.step_count; ++index) {
        if (turn->plan.steps[index].status == SI_AGENT_STEP_IN_PROGRESS) {
            return &turn->plan.steps[index];
        }
    }
    return NULL;
}

static bool agent_request_copy_fixed_text(
    char *destination, size_t destination_size,
    const char *source, size_t source_capacity)
{
    if (!destination || destination_size == 0U || !source ||
        source_capacity == 0U) {
        return false;
    }
    size_t length = 0U;
    while (length < source_capacity && source[length] != '\0') {
        ++length;
    }
    if (length == 0U || length >= source_capacity ||
        length >= destination_size) {
        return false;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return true;
}

/* Resolve the device-generated credential ID at every security boundary.
 * Conversation/thread IDs and capability snapshots are never accepted as a
 * substitute for current session liveness. Future delegated transports must
 * add their own generation-aware resolver before this function admits them. */
static bool agent_request_refresh_live_source(
    agent_request_bridge_binding_t *binding, char *error, size_t error_size)
{
    if (!binding ||
        binding->source_auth_kind != SI_AGENT_TASK_AUTH_DEVICE_SESSION ||
        (binding->origin != SI_AGENT_TASK_ORIGIN_WEB &&
         binding->origin != SI_AGENT_TASK_ORIGIN_MCP)) {
        if (error && error_size > 0) {
            strlcpy(error,
                    "Task source has no supported live authorization binding",
                    error_size);
        }
        return false;
    }
    si_auth_session_context_t live = {0};
    if (!si_auth_session_get_live_context(
            binding->source_auth_id, binding->source_auth_generation, &live) ||
        !live.authenticated || live.principal != binding->source_principal ||
        !si_capability_set_has(live.capabilities,
                               SI_CAPABILITY_AGENT_RUN)) {
        if (error && error_size > 0) {
            strlcpy(error,
                    "Task source session was revoked, expired, or changed",
                    error_size);
        }
        memset(&live, 0, sizeof(live));
        return false;
    }
    binding->live_authenticated = live.authenticated;
    binding->live_principal = live.principal;
    binding->live_auth_generation = live.generation;
    binding->live_capabilities = live.capabilities;
    memset(&live, 0, sizeof(live));
    return true;
}

static bool agent_request_build_binding(
    const char *job_id, const char *normalized_args_json,
    const char *idempotency_key, agent_request_bridge_binding_t *binding,
    char *error, size_t error_size)
{
    if (!job_id || !job_id[0] || !normalized_args_json ||
        !idempotency_key || !binding) {
        return false;
    }
    si_agent_task_service_snapshot_t *snapshot =
        agent_alloc(sizeof(*snapshot));
    if (!snapshot) {
        if (error && error_size > 0) {
            strlcpy(error, "Task Service snapshot allocation failed",
                    error_size);
        }
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    esp_err_t ret = si_agent_task_service_snapshot(snapshot);
    const si_agent_task_turn_t *turn =
        ret == ESP_OK && snapshot->runtime.has_active_turn ?
        &snapshot->runtime.active_turn : NULL;
    const si_agent_task_step_t *step = agent_request_current_step(turn);
    char thread_id[SI_AGENT_REQUEST_THREAD_ID_MAX_LEN + 1] = {0};
    char turn_id[SI_AGENT_REQUEST_TURN_ID_MAX_LEN + 1] = {0};
    char run_id[SI_AGENT_REQUEST_RUN_ID_MAX_LEN + 1] = {0};
    char step_id[SI_AGENT_REQUEST_STEP_ID_MAX_LEN + 1] = {0};
    char active_run_id[SI_AGENT_TASK_ID_MAX + 1] = {0};
    char device_id[SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN + 1] = {0};
    bool matches = agent_request_current_device_identity(device_id) &&
                   turn && step && snapshot->active.used;
    if (matches) {
        matches = agent_request_copy_fixed_text(
                      active_run_id, sizeof(active_run_id),
                      snapshot->active.run_id,
                      sizeof(snapshot->active.run_id)) &&
                  agent_request_copy_fixed_text(
                      thread_id, sizeof(thread_id), turn->thread_id,
                      sizeof(turn->thread_id)) &&
                  agent_request_copy_fixed_text(
                      turn_id, sizeof(turn_id), turn->turn_id,
                      sizeof(turn->turn_id)) &&
                  agent_request_copy_fixed_text(
                      run_id, sizeof(run_id),
                      turn->run_attempt.run_attempt_id,
                      sizeof(turn->run_attempt.run_attempt_id)) &&
                  agent_request_copy_fixed_text(
                      step_id, sizeof(step_id), step->step_id,
                      sizeof(step->step_id)) &&
                  strcmp(active_run_id, job_id) == 0 &&
                  strcmp(run_id, job_id) == 0;
    }
    if (matches) {
        memset(binding, 0, sizeof(*binding));
        strlcpy(binding->device_id, device_id,
                sizeof(binding->device_id));
        memcpy(binding->thread_id, thread_id, sizeof(thread_id));
        memcpy(binding->turn_id, turn_id, sizeof(turn_id));
        memcpy(binding->run_id, run_id, sizeof(run_id));
        memcpy(binding->step_id, step_id, sizeof(step_id));
        strlcpy(binding->normalized_args_json, normalized_args_json,
                sizeof(binding->normalized_args_json));
        strlcpy(binding->idempotency_key, idempotency_key,
                sizeof(binding->idempotency_key));
        binding->plan_version = turn->plan.version;
        binding->turn_status = turn->status;
        binding->origin = snapshot->active.origin;
        binding->source_auth_kind = snapshot->active.auth_kind;
        binding->source_principal = snapshot->active.source_principal;
        strlcpy(binding->source_auth_id,
                snapshot->active.source_auth_id,
                sizeof(binding->source_auth_id));
        binding->source_auth_generation =
            snapshot->active.source_auth_generation;
        binding->authority_ceiling = snapshot->active.authority_ceiling;
        matches = agent_request_refresh_live_source(binding, error,
                                                    error_size);
    } else if (error && error_size > 0 && !error[0]) {
        strlcpy(error,
                "Agent request is not bound to the active Task Plan step",
                error_size);
    }
    free(snapshot);
    return matches;
}

static si_agent_request_binding_input_t agent_request_binding_input(
    const agent_request_bridge_binding_t *binding)
{
    si_agent_request_binding_input_t input = {
        .device_id = binding->device_id,
        .thread_id = binding->thread_id,
        .turn_id = binding->turn_id,
        .run_id = binding->run_id,
        .step_id = binding->step_id,
        .plan_version = binding->plan_version,
        .normalized_args_json = binding->normalized_args_json,
        .idempotency_key = binding->idempotency_key,
    };
    return input;
}

static void agent_request_add_actor_json(
    cJSON *root, const char *name, const si_agent_request_actor_t *actor)
{
    if (!root || !name || !actor ||
        actor->principal == SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED) {
        return;
    }
    cJSON *item = cJSON_AddObjectToObject(root, name);
    cJSON_AddStringToObject(item, "principal",
                            si_agent_request_principal_name(actor->principal));
    cJSON_AddStringToObject(item, "session_id", actor->session_id);
    cJSON_AddStringToObject(item, "instance_id", actor->instance_id);
}

static void agent_request_add_json(cJSON *root,
                                   const si_agent_request_status_t *status)
{
    if (!root || !status) {
        return;
    }
    cJSON_AddStringToObject(root, "schema_version",
                            "exoanchor.agent_request.v2");
    cJSON_AddStringToObject(root, "request_id", status->request_id);
    cJSON_AddNumberToObject(root, "sequence", (double)status->sequence);
    cJSON_AddStringToObject(root, "device_id", status->device_id);
    cJSON_AddStringToObject(root, "thread_id", status->thread_id);
    cJSON_AddStringToObject(root, "turn_id", status->turn_id);
    cJSON_AddStringToObject(root, "run_id", status->run_id);
    cJSON_AddStringToObject(root, "step_id", status->step_id);
    cJSON_AddNumberToObject(root, "plan_version", status->plan_version);
    cJSON_AddStringToObject(root, "requester",
                            si_agent_request_principal_name(
                                status->requester.principal));
    agent_request_add_actor_json(root, "requester_actor",
                                 &status->requester);
    agent_request_add_actor_json(root, "reviewer_actor", &status->reviewer);
    agent_request_add_actor_json(root, "decider_actor", &status->decider);
    cJSON_AddBoolToObject(root, "reviewer_allowed",
                          status->reviewer_allowed);
    cJSON_AddStringToObject(root, "kind",
                            si_agent_request_kind_name(status->kind));
    cJSON_AddStringToObject(root, "resource", status->resource);
    cJSON_AddStringToObject(root, "reason", status->reason);
    cJSON_AddStringToObject(root, "risk",
                            si_agent_request_risk_name(status->risk));
    cJSON_AddStringToObject(root, "sensitivity",
                            status->kind == SI_AGENT_REQUEST_KIND_CONTEXT ?
                            "requested_source" : "action_parameters");
    cJSON_AddStringToObject(
        root, "requested_grant_scope",
        si_agent_request_grant_scope_name(status->requested_grant_scope));
    cJSON_AddStringToObject(
        root, "grant_scope",
        si_agent_request_grant_scope_name(status->grant_scope));
    cJSON_AddStringToObject(root, "status",
                            si_agent_request_state_name(status->state));
    cJSON_AddBoolToObject(root, "grant_consumed", status->grant_consumed);
    cJSON_AddBoolToObject(root, "lease_required",
                          status->lease_required);
    cJSON_AddNumberToObject(root, "created_ms", status->created_ms);
    cJSON_AddNumberToObject(root, "expires_at_ms",
                            status->expires_at_ms);
    uint32_t now_ms = si_monotonic_ms();
    bool expiry_relevant =
        status->state == SI_AGENT_REQUEST_REQUESTED ||
        status->state == SI_AGENT_REQUEST_EVALUATING ||
        status->state == SI_AGENT_REQUEST_REVIEWING ||
        status->state == SI_AGENT_REQUEST_WAITING_USER ||
        status->state == SI_AGENT_REQUEST_GRANTED;
    uint32_t remaining_ms =
        expiry_relevant &&
        (int32_t)(status->expires_at_ms - now_ms) > 0 ?
        status->expires_at_ms - now_ms : 0;
    cJSON_AddNumberToObject(root, "remaining_ms", remaining_ms);
    cJSON_AddStringToObject(root, "argument_hash",
                            status->normalized_args_hash);
    cJSON_AddStringToObject(root, "normalized_args_hash",
                            status->normalized_args_hash);
    cJSON_AddStringToObject(root, "idempotency_key",
                            status->idempotency_key);
    if (status->parent_grant_request_id[0]) {
        cJSON_AddStringToObject(root, "parent_grant_request_id",
                                status->parent_grant_request_id);
    }
    if (status->expected_effect[0]) {
        cJSON_AddStringToObject(root, "expected_effect",
                                status->expected_effect);
    }
    if (status->verification[0]) {
        cJSON_AddStringToObject(root, "verification",
                                status->verification);
    }
    cJSON *parameters = cJSON_Parse(status->params_json);
    cJSON_AddItemToObject(root, "parameters",
                          parameters ? parameters : cJSON_CreateObject());
    if (status->decision_source[0]) {
        cJSON_AddStringToObject(root, "decision_source",
                                status->decision_source);
    }
    if (status->decided_by_session[0]) {
        cJSON_AddStringToObject(root, "decided_by_session",
                                status->decided_by_session);
    }
    if (status->response[0]) {
        cJSON_AddStringToObject(root, "response", status->response);
    }
    if (status->decided_ms) {
        cJSON_AddNumberToObject(root, "decided_ms",
                                status->decided_ms);
    }
    if (status->consumed_ms) {
        cJSON_AddNumberToObject(root, "consumed_ms", status->consumed_ms);
    }
    if (status->verifying_ms) {
        cJSON_AddNumberToObject(root, "verifying_ms", status->verifying_ms);
    }
    if (status->completed_ms) {
        cJSON_AddNumberToObject(root, "completed_ms",
                                status->completed_ms);
    }
    if (status->verification_evidence[0]) {
        cJSON_AddStringToObject(root, "verification_evidence",
                                status->verification_evidence);
    }
    if (status->error[0]) {
        cJSON_AddStringToObject(root, "error", status->error);
    }
}

static esp_err_t agent_requests_get_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    char query[96] = {0};
    char run_id[SI_AGENT_REQUEST_RUN_ID_MAX_LEN + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "run_id", run_id,
                                    sizeof(run_id));
    }
    size_t count = 0;
    esp_err_t get_ret = si_agent_request_broker_enumerate(
        run_id[0] ? run_id : NULL, NULL, 0, &count);
    if (get_ret != ESP_OK && get_ret != ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Agent request queue unavailable");
    }
    si_agent_request_status_t *statuses = count > 0 ?
        agent_alloc(count * sizeof(*statuses)) : NULL;
    if (count > 0 && !statuses) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Agent request queue allocation failed");
    }
    if (count > 0) {
        memset(statuses, 0, count * sizeof(*statuses));
        size_t enumerated = 0;
        get_ret = si_agent_request_broker_enumerate(
            run_id[0] ? run_id : NULL, statuses, count, &enumerated);
        if (get_ret != ESP_OK || enumerated != count) {
            free(statuses);
            return httpd_resp_send_err(req,
                                       HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Agent request queue changed");
        }
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "schema_version",
                            "exoanchor.agent_request_queue.v2");
    cJSON *items = cJSON_AddArrayToObject(resp, "items");
    size_t pending_count = 0;
    size_t in_flight_count = 0;
    for (size_t index = 0; index < count; ++index) {
        const si_agent_request_status_t *status = &statuses[index];
        cJSON *item = cJSON_CreateObject();
        agent_request_add_json(item, status);
        cJSON_AddItemToArray(items, item);
        if (status->state == SI_AGENT_REQUEST_REVIEWING ||
            status->state == SI_AGENT_REQUEST_WAITING_USER) {
            ++pending_count;
        }
        if (status->state >= SI_AGENT_REQUEST_REQUESTED &&
            status->state <= SI_AGENT_REQUEST_VERIFYING) {
            ++in_flight_count;
        }
    }
    free(statuses);
    cJSON_AddNumberToObject(resp, "pending_count", pending_count);
    cJSON_AddNumberToObject(resp, "in_flight_count", in_flight_count);
    cJSON_AddNumberToObject(resp, "count", count);
    cJSON_AddStringToObject(resp, "owner", "device");
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t agent_request_decision_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return si_http_send_text_status(
            req, "403 Forbidden",
            "only an authenticated browser session may decide Agent requests");
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    const char *request_id =
        json_string_any(root, "request_id", "id", NULL);
    const char *decision =
        json_string_any(root, "decision", "action", NULL);
    const char *response =
        json_string_any(root, "response", "answer", "message");
    const char *grant_scope_text =
        json_string_any(root, "grant_scope", "scope", NULL);
    cJSON *approved_item =
        cJSON_GetObjectItemCaseSensitive(root, "approved");
    bool approved = (decision &&
                     (strcmp(decision, "approve") == 0 ||
                      strcmp(decision, "allow_once") == 0)) ||
                    cJSON_IsTrue(approved_item);
    if (!request_id || (!decision && !cJSON_IsBool(approved_item))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "request_id and decision are required");
    }
    si_agent_request_grant_scope_t grant_scope = SI_AGENT_REQUEST_GRANT_ONCE;
    if (grant_scope_text && strcmp(grant_scope_text, "once") != 0) {
        cJSON_Delete(root);
        return httpd_resp_send_err(
            req, HTTPD_400_BAD_REQUEST,
            "this bridge accepts only an exact one-time grant");
    }
    si_agent_request_decision_t request_decision = {
        .request_id = request_id,
        .approved = approved,
        .response = response,
        .grant_scope = grant_scope,
        .decider = {
            .principal = SI_AGENT_REQUEST_PRINCIPAL_BROWSER,
        },
    };
    strlcpy(request_decision.decider.session_id, session.session_id,
            sizeof(request_decision.decider.session_id));
    strlcpy(request_decision.decider.instance_id, "web-agent-review",
            sizeof(request_decision.decider.instance_id));
    si_agent_request_status_t status = {0};
    esp_err_t ret = si_agent_request_broker_decide_v2(
        &request_decision, &status);
    cJSON_Delete(root);
    if (ret == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "Agent request not found");
    }
    if (ret != ESP_OK) {
        return si_http_send_text_status(
            req, "409 Conflict",
            "Agent request is no longer waiting for a decision");
    }
    cJSON *resp = cJSON_CreateObject();
    agent_request_add_json(resp, &status);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static esp_err_t agent_request_cancel_handler(httpd_req_t *req)
{
    si_auth_session_context_t session = {0};
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, &session);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    if (session.principal != SI_PRINCIPAL_BROWSER) {
        return si_http_send_text_status(
            req, "403 Forbidden",
            "only an authenticated browser session may cancel Agent requests");
    }
    char buf[SCRATCH_BUFSIZE] = {0};
    cJSON *root = NULL;
    esp_err_t json_ret = si_http_recv_json(req, buf, sizeof(buf), &root);
    if (json_ret != ESP_OK) {
        return json_ret;
    }
    const char *request_id =
        json_string_any(root, "request_id", "id", NULL);
    if (!request_id) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "request_id is required");
    }
    si_agent_request_status_t status = {0};
    esp_err_t ret = si_agent_request_broker_cancel(
        request_id, session.session_id, &status);
    cJSON_Delete(root);
    if (ret == ESP_ERR_NOT_FOUND) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "Agent request not found");
    }
    if (ret != ESP_OK) {
        return si_http_send_text_status(
            req, "409 Conflict", "Agent request cannot be cancelled");
    }
    cJSON *resp = cJSON_CreateObject();
    agent_request_add_json(resp, &status);
    ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static void agent_context_catalog_add(cJSON *items, const char *source,
                                      const char *sensitivity,
                                      bool browser_provider,
                                      uint32_t max_bytes)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "source", source);
    cJSON_AddStringToObject(item, "sensitivity", sensitivity);
    cJSON_AddBoolToObject(item, "requestable", true);
    cJSON_AddBoolToObject(item, "browser_provider_required",
                          browser_provider);
    cJSON_AddNumberToObject(item, "max_bytes", max_bytes);
    cJSON_AddItemToArray(items, item);
}

static esp_err_t agent_context_catalog_handler(httpd_req_t *req)
{
    esp_err_t auth_ret =
        si_http_require_capability(req, SI_CAPABILITY_AGENT_RUN, NULL);
    if (auth_ret != ESP_OK) {
        return auth_ret;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "schema_version",
                            "exoanchor.context_catalog.v1");
    cJSON *items = cJSON_AddArrayToObject(resp, "context_catalog");
    agent_context_catalog_add(items, "device.status", "low", false, 8192);
    agent_context_catalog_add(items, "kvm.snapshot", "screen", false,
                              262144);
    agent_context_catalog_add(items, "terminal.selection", "medium", true,
                              8192);
    agent_context_catalog_add(items, "settings.unsaved_diff", "medium",
                              true, 8192);
    agent_context_catalog_add(items, "page.summary", "low", true, 4096);
    esp_err_t ret = si_http_send_json(req, resp);
    cJSON_Delete(resp);
    return ret;
}

static bool agent_request_authorize_ex(
    const char *job_id, const char *resource, const char *reason,
    const char *params_json, const char *expected_effect,
    const char *verification, si_agent_request_risk_t risk,
    bool lease_required, si_agent_request_kind_t kind,
    char *request_id, size_t request_id_size,
    char *response, size_t response_size,
    char *error, size_t error_size)
{
    if (request_id && request_id_size > 0) {
        request_id[0] = '\0';
    }
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (response && response_size > 0) {
        response[0] = '\0';
    }
    char normalized_args[SI_AGENT_REQUEST_PARAMS_MAX_LEN + 1] = {0};
    if (!agent_request_normalize_args(params_json, normalized_args,
                                      error, error_size)) {
        return false;
    }
    char policy_tool_name[SI_AGENT_TASK_TOOL_MAX_LEN + 1U] = {0};
    agent_request_policy_tool_name(resource, policy_tool_name);
    uint32_t expected_policy_revision = si_agent_tools_policy_revision();
    char policy_error[96] = {0};
    bool initial_policy_allowed = si_agent_tool_policy_allows(
        policy_tool_name, false, policy_error, sizeof(policy_error));
    if (!initial_policy_allowed ||
        expected_policy_revision != si_agent_tools_policy_revision()) {
        if (error && error_size > 0) {
            strlcpy(error,
                    policy_error[0] ? policy_error :
                    "Agent tool policy changed before request creation",
                    error_size);
        }
        return false;
    }
    char idempotency_key[SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN + 1] = {0};
    uint32_t request_sequence = ++s_agent_request_bridge_sequence;
    snprintf(idempotency_key, sizeof(idempotency_key),
             "bridge-v2-%08" PRIx32 "-%08" PRIx32,
             si_monotonic_ms(), request_sequence);

    agent_request_bridge_binding_t initial_binding = {0};
    if (!agent_request_build_binding(job_id, normalized_args,
                                     idempotency_key, &initial_binding,
                                     error, error_size)) {
        return false;
    }
    si_agent_access_mode_t access_mode = si_agent_access_mode_get();
    si_agent_request_v2_input_t input = {
        .binding = agent_request_binding_input(&initial_binding),
        .requester = {
            .principal = SI_AGENT_REQUEST_PRINCIPAL_AGENT,
        },
        .kind = kind,
        .resource = resource,
        .reason = reason,
        .expected_effect = expected_effect,
        .verification = verification,
        .risk = risk,
        .lease_required = lease_required,
        /* Human reviewer remains separate from trusted delegated policy. */
        .reviewer_allowed = false,
        .delegated_auto_approval =
            access_mode != SI_AGENT_ACCESS_MANUAL,
        .auto_approve_through =
            access_mode == SI_AGENT_ACCESS_FULL ?
            SI_AGENT_REQUEST_RISK_CRITICAL :
            SI_AGENT_REQUEST_RISK_MEDIUM,
        .requested_grant_scope = SI_AGENT_REQUEST_GRANT_ONCE,
        .ttl_ms = 120000U,
    };
    strlcpy(input.requester.session_id, AGENT_REQUEST_BRIDGE_SESSION_ID,
            sizeof(input.requester.session_id));
    strlcpy(input.requester.instance_id, AGENT_REQUEST_BRIDGE_INSTANCE_ID,
            sizeof(input.requester.instance_id));
    si_agent_request_status_t status = {0};
    esp_err_t ret = si_agent_request_broker_create_v2(&input, &status);
    if (ret != ESP_OK) {
        if (error && error_size > 0) {
            strlcpy(error,
                    ret == ESP_ERR_NO_MEM ?
                    "Agent request queue is full" :
                    esp_err_to_name(ret),
                    error_size);
        }
        return false;
    }
    if (request_id && request_id_size > 0) {
        strlcpy(request_id, status.request_id, request_id_size);
    }
    agent_run_add_action_eventf(
        job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
        status.state == SI_AGENT_REQUEST_GRANTED ?
        SI_ACTION_PHASE_STARTED : SI_ACTION_PHASE_WAITING_APPROVAL,
        false, true, "%s request · %s · %s",
        si_agent_request_risk_name(status.risk), status.resource,
        status.state == SI_AGENT_REQUEST_GRANTED ?
        "auto-granted by policy" : "waiting for browser decision");
    if (status.state != SI_AGENT_REQUEST_GRANTED &&
        si_agent_task_service_set_phase(
            job_id, SI_AGENT_TURN_WAITING_REQUEST) != ESP_OK) {
        (void)si_agent_request_broker_cancel(
            status.request_id, AGENT_REQUEST_BRIDGE_SESSION_ID, &status);
        if (error && error_size > 0) {
            strlcpy(error, "Task Service could not enter waiting-request",
                    error_size);
        }
        return false;
    }

    for (;;) {
        bool cancelled = false;
        bool paused = false;
        if (!s_agent_run_lock ||
            xSemaphoreTake(s_agent_run_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        } else {
            cancelled = !job_id ||
                        strcmp(s_agent_run_job.job_id, job_id) != 0 ||
                        s_agent_run_job.cancel_requested;
            paused = s_agent_run_job.pause_requested;
            if (!cancelled) {
                agent_run_state_t next_state = paused ?
                    AGENT_RUN_STATE_PAUSED :
                    AGENT_RUN_STATE_WAITING_REQUEST;
                const char *next_stage =
                    paused ? "已暂停" : "等待决定";
                const char *next_detail =
                    paused ?
                    "任务暂停；未决请求不会被自动批准" :
                    "精确请求已保存在设备端，关闭页面不会丢失";
                if (s_agent_run_job.state != next_state ||
                    strcmp(s_agent_run_job.stage, next_stage) != 0 ||
                    strcmp(s_agent_run_job.detail, next_detail) != 0) {
                    s_agent_run_job.state = next_state;
                    strlcpy(s_agent_run_job.stage, next_stage,
                            sizeof(s_agent_run_job.stage));
                    strlcpy(s_agent_run_job.detail, next_detail,
                            sizeof(s_agent_run_job.detail));
                    s_agent_run_job.updated_ms = si_monotonic_ms();
                }
            }
            xSemaphoreGive(s_agent_run_lock);
        }
        if (cancelled) {
            si_agent_request_broker_cancel_run(job_id);
            if (error && error_size > 0) {
                strlcpy(error, "run cancelled", error_size);
            }
            return false;
        }
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        ret = si_agent_request_broker_get_by_id(status.request_id, &status);
        if (ret != ESP_OK) {
            if (error && error_size > 0) {
                strlcpy(error, "request state unavailable", error_size);
            }
            return false;
        }
        if (status.state == SI_AGENT_REQUEST_GRANTED) {
            /* Re-read the Task Plan immediately before atomically consuming
             * the one-time grant. Replan, step, or argument drift expires it. */
            agent_request_bridge_binding_t consume_binding = {0};
            bool binding_ok = agent_request_build_binding(
                job_id, normalized_args, idempotency_key, &consume_binding,
                error, error_size);
            bool task_ready = binding_ok &&
                (consume_binding.turn_status == SI_AGENT_TURN_EXECUTING ||
                 (consume_binding.turn_status == SI_AGENT_TURN_WAITING_REQUEST &&
                  si_agent_task_service_set_phase(
                      job_id, SI_AGENT_TURN_EXECUTING) == ESP_OK));
            if (!task_ready) {
                (void)si_agent_request_broker_cancel(
                    status.request_id, AGENT_REQUEST_BRIDGE_SESSION_ID,
                    &status);
                if (error && error_size > 0 && !error[0]) {
                    strlcpy(error,
                            "Task Plan changed before grant consumption",
                            error_size);
                }
                return false;
            }
            si_agent_request_consume_t consume = {
                .request_id = status.request_id,
                .binding = agent_request_binding_input(&consume_binding),
            };
            si_control_lease_status_t expected_lease = {0};
            if (lease_required) {
                char lease_error[96] = {0};
                esp_err_t lease_ret = agent_run_acquire_kvm_control(
                    job_id, lease_error, sizeof(lease_error));
                if (lease_ret != ESP_OK) {
                    (void)si_agent_request_broker_cancel(
                        status.request_id, AGENT_REQUEST_BRIDGE_SESSION_ID,
                        &status);
                    if (error && error_size > 0) {
                        strlcpy(error,
                                lease_error[0] ? lease_error :
                                "Control lease unavailable before grant consumption",
                                error_size);
                    }
                    return false;
                }
            }
            si_control_lease_get_status(&expected_lease);
            ret = si_agent_request_broker_consume(&consume, &status);
            if (ret != ESP_OK) {
                if (error && error_size > 0) {
                    strlcpy(error,
                            status.error[0] ? status.error :
                            "Agent request grant could not be consumed",
                            error_size);
                }
                agent_run_add_action_eventf(
                    job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                    SI_ACTION_PHASE_FAILED, false, true,
                    "request grant rejected before effect · %s",
                    status.resource);
                return false;
            }
            si_control_lease_status_t current_lease = {0};
            si_control_lease_get_status(&current_lease);
            uint32_t current_policy_revision =
                si_agent_tools_policy_revision();
            char current_policy_error[96] = {0};
            bool current_policy_allowed = si_agent_tool_policy_allows(
                policy_tool_name, false, current_policy_error,
                sizeof(current_policy_error));
            uint32_t policy_revision_after_check =
                si_agent_tools_policy_revision();
            if (policy_revision_after_check != current_policy_revision) {
                current_policy_revision = policy_revision_after_check;
                current_policy_allowed = false;
            }
            uint32_t guard_now_32 = si_monotonic_ms();
            uint64_t guard_now_ms = guard_now_32;
            if (guard_now_ms == 0U) guard_now_ms = 1U;
            int32_t grant_remaining_ms =
                (int32_t)(status.expires_at_ms - guard_now_32);
            uint64_t guard_expires_at_ms = grant_remaining_ms > 0 ?
                guard_now_ms + (uint32_t)grant_remaining_ms : guard_now_ms;
            char current_device_id[SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN + 1] =
                {0};
            bool current_device_identity_available =
                agent_request_current_device_identity(current_device_id);
            bool exact_lease_held = !lease_required ||
                (current_lease.agent_active &&
                 strcmp(current_lease.agent_owner, "agent") == 0 &&
                 strcmp(current_lease.session_id, "embedded-agent") == 0);
            /* Broker consumption and policy/lease reads may take time. Resolve
             * the exact source session generation again at the Guard boundary
             * so logout/revoke/capability changes fail before action.started. */
            if (!agent_request_refresh_live_source(
                    &consume_binding, error, error_size)) {
                (void)si_agent_request_broker_finish(
                    status.request_id, false,
                    "source_session_not_live", &status);
                if (lease_required) agent_run_release_hid_control(job_id);
                agent_run_add_action_eventf(
                    job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                    SI_ACTION_PHASE_FAILED, false, true,
                    "request denied before effect · source session not live");
                return false;
            }
            si_agent_execution_intent_t execution_intent = {
                .contract_version = 1U,
                .thread_id = status.thread_id,
                .turn_id = status.turn_id,
                .run_id = status.run_id,
                .step_id = status.step_id,
                .action_id = status.request_id,
                .idempotency_key = status.idempotency_key,
                .tool_name = policy_tool_name,
                .normalized_args_hash = status.normalized_args_hash,
                .expected_target_identity = status.device_id,
                .current_target_identity =
                    current_device_identity_available ? current_device_id : "",
                .now_ms = guard_now_ms,
                .expires_at_ms = guard_expires_at_ms,
                .expected_policy_revision = expected_policy_revision,
                .current_policy_revision = current_policy_revision,
                .expected_lease_epoch = lease_required ?
                    expected_lease.epoch : 0U,
                .current_lease_epoch = lease_required ?
                    current_lease.epoch : 0U,
                .expected_auth_generation =
                    consume_binding.source_auth_generation,
                .current_auth_generation =
                    consume_binding.live_auth_generation,
                .authenticated = consume_binding.live_authenticated,
                .principal = consume_binding.live_principal,
                /* Current capabilities come from auth_service. The Task
                 * snapshot is only the immutable origin ceiling. */
                .principal_capabilities =
                    consume_binding.live_capabilities,
                .origin_authority_ceiling =
                    consume_binding.authority_ceiling,
                .required_capability =
                    agent_request_required_capability(policy_tool_name),
                .policy_allowed = current_policy_allowed,
                .lease_required = lease_required,
                .lease_held = exact_lease_held,
                .human_kvm_active = current_lease.kvm_active,
                .mode = agent_request_authorization_mode(),
                .risk = agent_request_authorization_risk(risk),
                .approval_required = true,
                .grant_consumed = status.grant_consumed,
            };
            si_agent_execution_guard_result_t guard =
                si_agent_execution_guard_evaluate(&execution_intent);
            if (guard.decision != SI_AGENT_EXECUTION_ALLOW) {
                (void)si_agent_request_broker_finish(
                    status.request_id, false, guard.reason, &status);
                if (lease_required) agent_run_release_hid_control(job_id);
                if (error && error_size > 0) {
                    strlcpy(error, guard.reason, error_size);
                }
                agent_run_add_action_eventf(
                    job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                    SI_ACTION_PHASE_FAILED, false, true,
                    "request denied by execution guard · %s",
                    si_agent_execution_decision_name(guard.decision));
                return false;
            }
            ret = si_agent_task_service_record_action_started(
                status.run_id, status.step_id, status.request_id,
                status.resource, status.normalized_args_hash,
                status.idempotency_key,
                kind == SI_AGENT_REQUEST_KIND_ACTION &&
                    (risk != SI_AGENT_REQUEST_RISK_LOW || lease_required));
            if (ret != ESP_OK) {
                (void)si_agent_request_broker_finish(
                    status.request_id, false,
                    "action_journal_failed", &status);
                if (error && error_size > 0) {
                    strlcpy(error,
                            "Action was not executed because its durable start record failed",
                            error_size);
                }
                agent_run_add_action_eventf(
                    job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                    SI_ACTION_PHASE_FAILED, false, true,
                    "request blocked before effect · action journal unavailable");
                return false;
            }
            if (response && response_size > 0) {
                strlcpy(response, status.response, response_size);
            }
            if (xSemaphoreTake(s_agent_run_lock,
                               pdMS_TO_TICKS(100)) == pdTRUE) {
                if (strcmp(s_agent_run_job.job_id, job_id) == 0) {
                    s_agent_run_job.state = AGENT_RUN_STATE_RUNNING;
                    strlcpy(s_agent_run_job.stage, "继续执行",
                            sizeof(s_agent_run_job.stage));
                    strlcpy(s_agent_run_job.detail,
                            "一次性请求已批准，正在恢复原任务步骤",
                            sizeof(s_agent_run_job.detail));
                    s_agent_run_job.updated_ms = si_monotonic_ms();
                }
                xSemaphoreGive(s_agent_run_lock);
            }
            agent_run_add_action_eventf(
                job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                SI_ACTION_PHASE_STARTED, false, true,
                "request approved once · %s · source=%s",
                status.resource,
                status.decision_source[0] ? status.decision_source :
                                            "browser");
            return true;
        }
        if (status.state != SI_AGENT_REQUEST_REQUESTED &&
            status.state != SI_AGENT_REQUEST_EVALUATING &&
            status.state != SI_AGENT_REQUEST_REVIEWING &&
            status.state != SI_AGENT_REQUEST_WAITING_USER) {
            if (error && error_size > 0) {
                strlcpy(error,
                        status.error[0] ? status.error :
                        si_agent_request_state_name(status.state),
                        error_size);
            }
            agent_run_add_action_eventf(
                job_id, status.request_id, SI_ACTION_CHANNEL_AGENT,
                status.state == SI_AGENT_REQUEST_EXPIRED ?
                SI_ACTION_PHASE_FAILED : SI_ACTION_PHASE_CANCELLED,
                false, true, "request %s · %s",
                si_agent_request_state_name(status.state),
                status.resource);
            if (xSemaphoreTake(s_agent_run_lock,
                               pdMS_TO_TICKS(100)) == pdTRUE) {
                if (strcmp(s_agent_run_job.job_id, job_id) == 0) {
                    s_agent_run_job.state = AGENT_RUN_STATE_RUNNING;
                }
                xSemaphoreGive(s_agent_run_lock);
            }
            (void)si_agent_task_service_set_phase(
                job_id, SI_AGENT_TURN_EXECUTING);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static bool agent_request_authorize(
    const char *job_id, const char *resource, const char *reason,
    const char *params_json, const char *expected_effect,
    const char *verification, si_agent_request_risk_t risk,
    bool lease_required, char *request_id, size_t request_id_size,
    char *error, size_t error_size)
{
    return agent_request_authorize_ex(
        job_id, resource, reason, params_json, expected_effect,
        verification, risk, lease_required, SI_AGENT_REQUEST_KIND_ACTION,
        request_id, request_id_size, NULL, 0, error, error_size);
}

static void agent_request_finish(const char *job_id,
                                 const char *request_id,
                                 bool ok, const char *error)
{
    if (!request_id || !request_id[0]) {
        return;
    }
    si_agent_request_status_t status = {0};
    if (si_agent_request_broker_get_by_id(request_id, &status) != ESP_OK) {
        return;
    }
    esp_err_t journal_ret = si_agent_task_service_record_action_result(
        status.run_id, status.step_id, status.request_id, ok, NULL,
        ok ? "execution_returned" : "execution_failed");
    if (journal_ret != ESP_OK) {
        (void)si_agent_request_broker_mark_unknown(
            request_id, "action_result_journal_failed", &status);
        (void)si_agent_task_service_mark_outcome_unknown(
            job_id, request_id, "action_result_journal_failed");
        agent_run_add_action_eventf(
            job_id, request_id, SI_ACTION_CHANNEL_AGENT,
            SI_ACTION_PHASE_FAILED, false, true,
            "request outcome unknown · result journal unavailable");
        return;
    }
    char broker_error[SI_AGENT_REQUEST_ERROR_MAX_LEN + 1] = {0};
    strlcpy(broker_error,
            error && error[0] ? error : "request execution failed",
            sizeof(broker_error));
    esp_err_t ret = ok ?
        si_agent_request_broker_mark_verifying(request_id, &status) :
        si_agent_request_broker_finish(request_id, false, broker_error,
                                       &status);
    if (ret != ESP_OK) {
        return;
    }
    agent_run_add_action_eventf(
        job_id, request_id, SI_ACTION_CHANNEL_AGENT,
        ok ? SI_ACTION_PHASE_STARTED : SI_ACTION_PHASE_FAILED,
        false, true, "request %s · %s",
        ok ? "effect returned; independent verification pending" :
             "execution failed",
        status.resource);
}
