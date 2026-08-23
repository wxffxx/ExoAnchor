// Types owned by the optional embedded Agent runtime.
#include "agent_task_service.h"
#include "agent_verifier.h"
#include "agent_execution_guard.h"
#define AGENT_RUNTIME_V2_PLAN_ID "plan-executor-v1"
#define AGENT_RUNTIME_V2_STEP_ID "step-executor-v1"
typedef si_agent_api_profile_t agent_api_profile_t;

typedef struct {
    bool used;
    bool deleted;
    char id[AGENT_SESSION_ID_MAX_LEN + 1];
    char title[AGENT_SESSION_TITLE_MAX_LEN + 1];
    uint32_t created_ms;
    uint32_t updated_ms;
    uint32_t message_count;
} agent_session_summary_t;

typedef enum {
    AGENT_RUN_STATE_IDLE = 0,
    AGENT_RUN_STATE_RUNNING,
    AGENT_RUN_STATE_WAITING_REQUEST,
    AGENT_RUN_STATE_PAUSED,
    AGENT_RUN_STATE_DONE,
    AGENT_RUN_STATE_FAILED,
    AGENT_RUN_STATE_ABORTED,
    AGENT_RUN_STATE_OUTCOME_UNKNOWN,
} agent_run_state_t;

typedef struct {
    agent_run_state_t state;
    bool pause_requested, cancel_requested, recovered_after_reboot;
    char job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1];
    char turn_id[AGENT_TURN_ID_MAX_LEN + 1];
    /* Legacy Web projection: conversation/thread ID, never auth identity. */
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    si_agent_task_origin_t source_origin;
    si_agent_task_auth_kind_t source_auth_kind;
    si_principal_kind_t source_principal;
    uint32_t source_auth_generation;
    si_capability_set_t source_authority_ceiling;
    char source_auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U];
    /* Transport-supplied idempotency is retained only in volatile Run RAM so
     * an HTTP retry can bind to this exact execution without launching a
     * second legacy worker. It is never projected into status or history. */
    char idempotency_key[SI_AGENT_TASK_IDEMPOTENCY_MAX_LEN + 1U];
    char idempotency_fingerprint[65];
    char goal[AGENT_RUN_GOAL_MAX_LEN + 1];
    char pending_steer[AGENT_RUN_STEER_MAX_LEN + 1];
    uint32_t intent_revision;
    uint32_t plan_version;
    char profile[AGENT_API_PROFILE_ID_MAX_LEN + 1];
    char model[AGENT_API_MODEL_MAX_LEN + 1];
    bool dry_run;
    bool include_screenshot;
    bool allow_web_search;
    uint32_t started_ms;
    uint32_t updated_ms;
    uint32_t finished_ms;
    char stage[AGENT_RUN_STAGE_MAX_LEN + 1];
    char detail[AGENT_RUN_DETAIL_MAX_LEN + 1];
    char error[AGENT_RUN_ERROR_MAX_LEN + 1];
    /* Runtime V2 is a shadow/safety binding until its execution gates pass. */
    bool task_service_bound;
    /* Fail-closed while a pause/abort journal transition runs without holding
     * the Web Run lock. Finalizer/dispatcher treat pending as degraded. */
    bool task_service_control_pending;
    uint32_t control_epoch;
    char task_service_error[AGENT_RUN_ERROR_MAX_LEN + 1];
    char page_context_json[AGENT_PAGE_CONTEXT_MAX_LEN + 1];
    char *result_json;
    TaskHandle_t task;
    /* Server-issued report-owner capability for this exact run attempt. */
    si_hid_owner_token_t hid_owner;
    uint32_t next_action_id;
    si_action_event_buffer_t action_events;
} agent_run_job_t;

typedef struct {
    char job_id[AGENT_RUN_JOB_ID_MAX_LEN + 1];
    char turn_id[AGENT_TURN_ID_MAX_LEN + 1];
    /* Legacy Web projection: conversation/thread ID, never auth identity. */
    char session_id[AGENT_SESSION_ID_MAX_LEN + 1];
    si_agent_task_origin_t source_origin;
    si_agent_task_auth_kind_t source_auth_kind;
    si_principal_kind_t source_principal;
    uint32_t source_auth_generation;
    si_capability_set_t source_authority_ceiling;
    char source_auth_id[SI_AGENT_TASK_SOURCE_ID_MAX_LEN + 1U];
    char profile[AGENT_API_PROFILE_ID_MAX_LEN + 1];
    char model[AGENT_API_MODEL_MAX_LEN + 1];
    bool dry_run;
    bool include_screenshot;
    bool allow_web_search;
    bool task_service_bound;
    /* Nonzero only after the H.264/Agent internal-memory arbiter granted this
     * exact executor launch. The generation-bound token prevents stale or
     * duplicate cleanup from releasing a newer owner. */
    uint32_t h264_resource_token;
    char page_context_json[AGENT_PAGE_CONTEXT_MAX_LEN + 1];
    char *message;
} agent_run_task_arg_t;
