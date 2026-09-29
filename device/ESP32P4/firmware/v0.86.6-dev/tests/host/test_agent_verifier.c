#include "agent_verifier.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *const HASH_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *const HASH_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static si_agent_verification_spec_t spec(void)
{
    return (si_agent_verification_spec_t){
        .mutation = true,
        .require_readback = false,
        .action_started_ms = 100,
        .action_id = "act-1",
        .target_identity = "device:board-a/host-a",
        .completion_criteria_hash = HASH_A,
        .minimum_generation = 7,
        .required_issuer = SI_AGENT_EVIDENCE_ISSUER_DEVICE_OBSERVER,
    };
}

int main(void)
{
    si_agent_verification_spec_t request = spec();
    si_agent_verification_result_t result =
        si_agent_verify(&request, NULL, 0);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t self_report = {
        .kind = SI_AGENT_EVIDENCE_MODEL_ASSERTION,
        .outcome = SI_AGENT_EVIDENCE_OUTCOME_MATCH,
        .captured_ms = 110,
        .evidence_id = "model-1",
        .action_id = "act-1",
        .target_identity = "device:board-a/host-a",
        .completion_criteria_hash = HASH_A,
        .generation = 7,
        .issuer = SI_AGENT_EVIDENCE_ISSUER_MODEL,
    };
    result = si_agent_verify(&request, &self_report, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t stale = {
        .kind = SI_AGENT_EVIDENCE_OBSERVATION,
        .outcome = SI_AGENT_EVIDENCE_OUTCOME_MATCH,
        .captured_ms = 99,
        .evidence_id = "obs-stale",
        .action_id = "act-1",
        .target_identity = "device:board-a/host-a",
        .completion_criteria_hash = HASH_A,
        .generation = 7,
        .issuer = SI_AGENT_EVIDENCE_ISSUER_DEVICE_OBSERVER,
    };
    result = si_agent_verify(&request, &stale, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t wrong_target = stale;
    wrong_target.captured_ms = 101;
    wrong_target.target_identity = "device:board-b/host-a";
    result = si_agent_verify(&request, &wrong_target, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t wrong_criteria = stale;
    wrong_criteria.captured_ms = 101;
    wrong_criteria.completion_criteria_hash = HASH_B;
    result = si_agent_verify(&request, &wrong_criteria, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t same_boundary = stale;
    same_boundary.captured_ms = request.action_started_ms;
    result = si_agent_verify(&request, &same_boundary, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t old_generation = stale;
    old_generation.captured_ms = 101;
    old_generation.generation = 6;
    result = si_agent_verify(&request, &old_generation, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t observation = stale;
    observation.captured_ms = 101;
    observation.evidence_id = "obs-1";
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_PASSED);
    assert(strcmp(result.evidence_id, "obs-1") == 0);

    observation.outcome = SI_AGENT_EVIDENCE_OUTCOME_MISMATCH;
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_FAILED);

    observation.outcome = SI_AGENT_EVIDENCE_OUTCOME_UNKNOWN;
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_UNKNOWN);

    request.require_readback = true;
    observation.outcome = SI_AGENT_EVIDENCE_OUTCOME_MATCH;
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);
    observation.kind = SI_AGENT_EVIDENCE_READBACK;
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_PASSED);

    request.required_issuer = SI_AGENT_EVIDENCE_ISSUER_DEVICE_MANAGER;
    result = si_agent_verify(&request, &observation, 1);
    assert(result.status == SI_AGENT_VERIFY_PENDING);

    si_agent_evidence_t bad_artifact = observation;
    request.require_readback = false;
    request.required_issuer = SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE;
    bad_artifact.kind = SI_AGENT_EVIDENCE_ARTIFACT;
    bad_artifact.issuer = SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE;
    bad_artifact.artifact_hash = "not-a-sha256";
    result = si_agent_verify(&request, &bad_artifact, 1);
    assert(result.status == SI_AGENT_VERIFY_INVALID);

    puts("agent verifier tests: PASS");
    return 0;
}
