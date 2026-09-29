// Forward contract used by modules that are assembled before the runtime.
static esp_err_t resource_operation_start(void);
static bool resource_operation_begin(
    si_resource_operation_resource_t resource,
    si_resource_operation_actor_t actor, const char *operation_id,
    const char *run_id, const char *label, bool cancellable,
    si_resource_operation_token_t *token);
static bool resource_operation_finish(
    const si_resource_operation_token_t *token);
static bool resource_operation_cancel_requested(
    const si_resource_operation_token_t *token);
static bool resource_operation_manual_allowed(
    si_resource_operation_resource_t resource);
static bool resource_operation_get(
    si_resource_operation_resource_t resource,
    si_resource_operation_status_t *status);
static bool automation_manual_power_allowed(void);
static bool automation_agent_run_active(void);
static bool automation_abort_active_agent(const char *expected_run_id);
static bool automation_request_stop_actor(
    si_resource_operation_actor_t actor, const char *reason);
static bool automation_request_stop_all(const char *reason);
static bool automation_is_quiescent(void);
static bool automation_begin_maintenance(
    const char *operation_id, const char *label, bool cancellable,
    si_resource_operation_token_t *token);
