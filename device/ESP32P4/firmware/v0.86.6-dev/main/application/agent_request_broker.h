#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SI_AGENT_REQUEST_QUEUE_CAPACITY 8U

#define SI_AGENT_REQUEST_ID_MAX_LEN 24
#define SI_AGENT_REQUEST_RUN_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_THREAD_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_TURN_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_STEP_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN 64
#define SI_AGENT_REQUEST_RESOURCE_MAX_LEN 40
#define SI_AGENT_REQUEST_REASON_MAX_LEN 160
#define SI_AGENT_REQUEST_EFFECT_MAX_LEN 160
#define SI_AGENT_REQUEST_VERIFY_MAX_LEN 120
#define SI_AGENT_REQUEST_PARAMS_MAX_LEN 768
#define SI_AGENT_REQUEST_HASH_HEX_LEN 64
#define SI_AGENT_REQUEST_SESSION_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_INSTANCE_ID_MAX_LEN 40
#define SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN 64
#define SI_AGENT_REQUEST_RESPONSE_MAX_LEN 512
#define SI_AGENT_REQUEST_ERROR_MAX_LEN 128

#define SI_AGENT_REQUEST_SNAPSHOT_SCHEMA_VERSION 2U

typedef enum {
    SI_AGENT_REQUEST_KIND_CONTEXT = 0,
    SI_AGENT_REQUEST_KIND_ACTION,
} si_agent_request_kind_t;

typedef enum {
    SI_AGENT_REQUEST_RISK_LOW = 0,
    SI_AGENT_REQUEST_RISK_MEDIUM,
    SI_AGENT_REQUEST_RISK_HIGH,
    SI_AGENT_REQUEST_RISK_CRITICAL,
} si_agent_request_risk_t;

/* GRANTED is approval only. An executor must atomically consume it first. */
typedef enum {
    SI_AGENT_REQUEST_EMPTY = 0,
    SI_AGENT_REQUEST_REQUESTED,
    SI_AGENT_REQUEST_EVALUATING,
    SI_AGENT_REQUEST_REVIEWING,
    SI_AGENT_REQUEST_WAITING_USER,
    SI_AGENT_REQUEST_GRANTED,
    SI_AGENT_REQUEST_EXECUTING,
    SI_AGENT_REQUEST_VERIFYING,
    SI_AGENT_REQUEST_COMPLETED,
    SI_AGENT_REQUEST_FAILED,
    SI_AGENT_REQUEST_DENIED,
    SI_AGENT_REQUEST_EXPIRED,
    SI_AGENT_REQUEST_CANCELLED,
    SI_AGENT_REQUEST_UNKNOWN,
} si_agent_request_state_t;

typedef enum {
    SI_AGENT_REQUEST_GRANT_ONCE = 0,
    SI_AGENT_REQUEST_GRANT_STEP,
    SI_AGENT_REQUEST_GRANT_RUN,
    SI_AGENT_REQUEST_GRANT_THREAD,
} si_agent_request_grant_scope_t;

typedef enum {
    SI_AGENT_REQUEST_PRINCIPAL_UNSPECIFIED = 0,
    SI_AGENT_REQUEST_PRINCIPAL_AGENT,
    SI_AGENT_REQUEST_PRINCIPAL_BROWSER,
    SI_AGENT_REQUEST_PRINCIPAL_REVIEWER,
    SI_AGENT_REQUEST_PRINCIPAL_POLICY,
    SI_AGENT_REQUEST_PRINCIPAL_RUNTIME,
} si_agent_request_principal_t;

typedef struct {
    si_agent_request_principal_t principal;
    char session_id[SI_AGENT_REQUEST_SESSION_ID_MAX_LEN + 1];
    char instance_id[SI_AGENT_REQUEST_INSTANCE_ID_MAX_LEN + 1];
} si_agent_request_actor_t;

typedef struct {
    const char *device_id;
    const char *thread_id;
    const char *turn_id;
    const char *run_id;
    const char *step_id;
    uint32_t plan_version;
    /* The caller owns canonical JSON normalization before entering Broker. */
    const char *normalized_args_json;
    const char *idempotency_key;
} si_agent_request_binding_input_t;

/* Legacy input retained while old embedded-Agent callers migrate. */
typedef struct {
    const char *run_id;
    si_agent_request_kind_t kind;
    const char *resource;
    const char *reason;
    const char *params_json;
    const char *expected_effect;
    const char *verification;
    si_agent_request_risk_t risk;
    bool lease_required;
    uint32_t ttl_ms;
} si_agent_request_input_t;

typedef struct {
    si_agent_request_binding_input_t binding;
    /* Trusted Task Service identity; never populate from model arguments. */
    si_agent_request_actor_t requester;
    si_agent_request_kind_t kind;
    const char *resource;
    const char *reason;
    const char *expected_effect;
    const char *verification;
    si_agent_request_risk_t risk;
    bool lease_required;
    /* Derived from the user's delegation policy, never from the requester. */
    bool reviewer_allowed;
    /*
     * Trusted device-side delegation. The model and external clients never
     * populate these fields; the Task Service derives them from the persisted
     * browser-selected access mode.
     */
    bool delegated_auto_approval;
    si_agent_request_risk_t auto_approve_through;
    si_agent_request_grant_scope_t requested_grant_scope;
    uint32_t ttl_ms;
} si_agent_request_v2_input_t;

typedef struct {
    si_agent_request_state_t state;
    si_agent_request_kind_t kind;
    si_agent_request_risk_t risk;
    bool lease_required;
    bool reviewer_allowed;
    bool grant_consumed;
    si_agent_request_grant_scope_t requested_grant_scope;
    si_agent_request_grant_scope_t grant_scope;
    uint32_t created_ms;
    uint32_t expires_at_ms;
    uint32_t decided_ms;
    uint32_t consumed_ms;
    uint32_t verifying_ms;
    uint32_t completed_ms;
    uint32_t plan_version;
    uint64_t sequence;
    char request_id[SI_AGENT_REQUEST_ID_MAX_LEN + 1];
    char parent_grant_request_id[SI_AGENT_REQUEST_ID_MAX_LEN + 1];
    char device_id[SI_AGENT_REQUEST_DEVICE_ID_MAX_LEN + 1];
    char thread_id[SI_AGENT_REQUEST_THREAD_ID_MAX_LEN + 1];
    char turn_id[SI_AGENT_REQUEST_TURN_ID_MAX_LEN + 1];
    char run_id[SI_AGENT_REQUEST_RUN_ID_MAX_LEN + 1];
    char step_id[SI_AGENT_REQUEST_STEP_ID_MAX_LEN + 1];
    char resource[SI_AGENT_REQUEST_RESOURCE_MAX_LEN + 1];
    char reason[SI_AGENT_REQUEST_REASON_MAX_LEN + 1];
    char params_json[SI_AGENT_REQUEST_PARAMS_MAX_LEN + 1];
    /* params_hash is retained as a legacy alias. */
    char params_hash[SI_AGENT_REQUEST_HASH_HEX_LEN + 1];
    char normalized_args_hash[SI_AGENT_REQUEST_HASH_HEX_LEN + 1];
    char idempotency_key[SI_AGENT_REQUEST_IDEMPOTENCY_KEY_MAX_LEN + 1];
    char expected_effect[SI_AGENT_REQUEST_EFFECT_MAX_LEN + 1];
    char verification[SI_AGENT_REQUEST_VERIFY_MAX_LEN + 1];
    si_agent_request_actor_t requester;
    si_agent_request_actor_t reviewer;
    si_agent_request_actor_t decider;
    char decision_source[24];
    /* Legacy projection for existing HTTP JSON. */
    char decided_by_session[SI_AGENT_REQUEST_SESSION_ID_MAX_LEN + 1];
    /* Approval/materialization response and verification evidence are distinct. */
    char response[SI_AGENT_REQUEST_RESPONSE_MAX_LEN + 1];
    char verification_evidence[SI_AGENT_REQUEST_RESPONSE_MAX_LEN + 1];
    char error[SI_AGENT_REQUEST_ERROR_MAX_LEN + 1];
} si_agent_request_status_t;

typedef struct {
    const char *request_id;
    bool approved;
    const char *response;
    si_agent_request_actor_t decider;
    si_agent_request_grant_scope_t grant_scope;
} si_agent_request_decision_t;

typedef struct {
    const char *request_id;
    si_agent_request_binding_input_t binding;
} si_agent_request_consume_t;

typedef uint32_t (*si_agent_request_now_fn_t)(void *context);

typedef struct {
    si_agent_request_status_t status;
    uint32_t remaining_ttl_ms;
} si_agent_request_snapshot_entry_t;

typedef struct {
    uint32_t schema_version;
    uint32_t count;
    uint64_t next_sequence;
    si_agent_request_snapshot_entry_t entries[SI_AGENT_REQUEST_QUEUE_CAPACITY];
} si_agent_request_snapshot_t;

/* Snapshots are large; firmware callers should allocate them from PSRAM. */

esp_err_t si_agent_request_broker_start(void);
esp_err_t si_agent_request_broker_set_now_provider(
    si_agent_request_now_fn_t now_fn, void *context);

esp_err_t si_agent_request_broker_create_v2(
    const si_agent_request_v2_input_t *input,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_get_by_id(
    const char *request_id, si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_enumerate(
    const char *run_id, si_agent_request_status_t *out,
    size_t capacity, size_t *out_count);
esp_err_t si_agent_request_broker_decide_v2(
    const si_agent_request_decision_t *decision,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_consume(
    const si_agent_request_consume_t *consume,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_mark_verifying(
    const char *request_id, si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_complete_verified(
    const char *request_id, const char *verification_evidence,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_mark_unknown(
    const char *request_id, const char *error,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_export_snapshot(
    si_agent_request_snapshot_t *out);
esp_err_t si_agent_request_broker_import_snapshot(
    const si_agent_request_snapshot_t *snapshot);

/*
 * Atomically clears only records bound to run_id after every matching record
 * is terminal. Callers must first durably record the owning Task terminal.
 * No matching records is an idempotent success.
 */
esp_err_t si_agent_request_broker_retire_run(const char *run_id);

/*
 * After the caller has durably recorded the owning Task as terminal, this
 * atomically terminalizes and clears every Broker record for run_id under one
 * Broker lock. Pre-effect requests become CANCELLED; requests whose effect may
 * have started become UNKNOWN. No matching records is an idempotent success.
 */
esp_err_t si_agent_request_broker_terminalize_and_retire_run(
    const char *run_id);

/* Legacy API. New execution paths must call consume before an effect. */
esp_err_t si_agent_request_broker_create(
    const si_agent_request_input_t *input, si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_get(
    const char *run_id, si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_decide(
    const char *request_id, bool approved, const char *response,
    const char *session_id,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_cancel(
    const char *request_id, const char *session_id,
    si_agent_request_status_t *out);
esp_err_t si_agent_request_broker_finish(
    const char *request_id, bool ok, const char *error,
    si_agent_request_status_t *out);
void si_agent_request_broker_cancel_run(const char *run_id);

const char *si_agent_request_kind_name(si_agent_request_kind_t kind);
const char *si_agent_request_risk_name(si_agent_request_risk_t risk);
const char *si_agent_request_state_name(si_agent_request_state_t state);
const char *si_agent_request_grant_scope_name(
    si_agent_request_grant_scope_t scope);
const char *si_agent_request_principal_name(
    si_agent_request_principal_t principal);
