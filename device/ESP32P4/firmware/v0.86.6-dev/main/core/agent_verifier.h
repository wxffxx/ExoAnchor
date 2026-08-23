#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deterministic completion gate for Agent actions.
 *
 * A successful tool return, HTTP status, process exit code or model statement
 * is never completion evidence.  Callers must provide a fresh, target-bound
 * observation, artifact or deterministic read-back produced after the action.
 * This module is pure and owns no device state.
 */

#define SI_AGENT_VERIFY_ID_MAX_LEN 64U
#define SI_AGENT_VERIFY_TARGET_MAX_LEN 96U
#define SI_AGENT_VERIFY_HASH_MAX_LEN 64U
#define SI_AGENT_VERIFY_REASON_MAX_LEN 128U

typedef enum {
    SI_AGENT_EVIDENCE_ISSUER_UNSPECIFIED = 0,
    SI_AGENT_EVIDENCE_ISSUER_DEVICE_MANAGER,
    SI_AGENT_EVIDENCE_ISSUER_DEVICE_OBSERVER,
    SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE,
    SI_AGENT_EVIDENCE_ISSUER_TOOL_ADAPTER,
    SI_AGENT_EVIDENCE_ISSUER_MODEL,
} si_agent_evidence_issuer_t;

typedef enum {
    SI_AGENT_EVIDENCE_NONE = 0,
    SI_AGENT_EVIDENCE_OBSERVATION,
    SI_AGENT_EVIDENCE_ARTIFACT,
    SI_AGENT_EVIDENCE_READBACK,
    SI_AGENT_EVIDENCE_TOOL_RETURN,
    SI_AGENT_EVIDENCE_MODEL_ASSERTION,
} si_agent_evidence_kind_t;

typedef enum {
    SI_AGENT_EVIDENCE_OUTCOME_UNKNOWN = 0,
    SI_AGENT_EVIDENCE_OUTCOME_MATCH,
    SI_AGENT_EVIDENCE_OUTCOME_MISMATCH,
} si_agent_evidence_outcome_t;

typedef enum {
    SI_AGENT_VERIFY_PENDING = 0,
    SI_AGENT_VERIFY_PASSED,
    SI_AGENT_VERIFY_FAILED,
    SI_AGENT_VERIFY_UNKNOWN,
    SI_AGENT_VERIFY_INVALID,
} si_agent_verification_verdict_t;

typedef struct {
    bool mutation;
    bool require_readback;
    uint64_t action_started_ms;
    const char *action_id;
    const char *target_identity;
    const char *completion_criteria_hash;
    uint64_t minimum_generation;
    si_agent_evidence_issuer_t required_issuer;
} si_agent_verification_spec_t;

typedef struct {
    si_agent_evidence_kind_t kind;
    si_agent_evidence_outcome_t outcome;
    uint64_t captured_ms;
    const char *evidence_id;
    const char *artifact_hash;
    const char *action_id;
    const char *target_identity;
    const char *completion_criteria_hash;
    uint64_t generation;
    si_agent_evidence_issuer_t issuer;
} si_agent_evidence_t;

typedef struct {
    si_agent_verification_verdict_t status;
    si_agent_evidence_kind_t accepted_kind;
    size_t accepted_index;
    char evidence_id[SI_AGENT_VERIFY_ID_MAX_LEN + 1U];
    char reason[SI_AGENT_VERIFY_REASON_MAX_LEN + 1U];
} si_agent_verification_result_t;

si_agent_verification_result_t si_agent_verify(
    const si_agent_verification_spec_t *spec,
    const si_agent_evidence_t *evidence,
    size_t evidence_count);

bool si_agent_evidence_kind_is_authoritative(si_agent_evidence_kind_t kind);
bool si_agent_evidence_issuer_is_authoritative(
    si_agent_evidence_issuer_t issuer);
const char *si_agent_evidence_kind_name(si_agent_evidence_kind_t kind);
const char *si_agent_verification_status_name(
    si_agent_verification_verdict_t status);

#ifdef __cplusplus
}
#endif
