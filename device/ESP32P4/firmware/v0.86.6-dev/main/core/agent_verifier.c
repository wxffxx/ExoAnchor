#include "agent_verifier.h"

#include <stdio.h>
#include <string.h>

static bool bounded_nonempty(const char *value, size_t max_len)
{
    if (!value || !value[0]) {
        return false;
    }
    return strnlen(value, max_len + 1U) <= max_len;
}

static bool same_required(const char *expected, const char *actual,
                          size_t max_len)
{
    return bounded_nonempty(expected, max_len) &&
           bounded_nonempty(actual, max_len) &&
           strcmp(expected, actual) == 0;
}

static bool sha256_hex_valid(const char *value)
{
    if (!value || strnlen(value, SI_AGENT_VERIFY_HASH_MAX_LEN + 1U) !=
                      SI_AGENT_VERIFY_HASH_MAX_LEN) {
        return false;
    }
    for (size_t index = 0; index < SI_AGENT_VERIFY_HASH_MAX_LEN; ++index) {
        char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void set_reason(si_agent_verification_result_t *result,
                       const char *reason)
{
    if (!result) {
        return;
    }
    snprintf(result->reason, sizeof(result->reason), "%s",
             reason ? reason : "");
}

bool si_agent_evidence_kind_is_authoritative(si_agent_evidence_kind_t kind)
{
    return kind == SI_AGENT_EVIDENCE_OBSERVATION ||
           kind == SI_AGENT_EVIDENCE_ARTIFACT ||
           kind == SI_AGENT_EVIDENCE_READBACK;
}

bool si_agent_evidence_issuer_is_authoritative(
    si_agent_evidence_issuer_t issuer)
{
    return issuer == SI_AGENT_EVIDENCE_ISSUER_DEVICE_MANAGER ||
           issuer == SI_AGENT_EVIDENCE_ISSUER_DEVICE_OBSERVER ||
           issuer == SI_AGENT_EVIDENCE_ISSUER_ARTIFACT_STORE;
}

static bool evidence_shape_valid(const si_agent_evidence_t *item)
{
    if (!item ||
        item->kind <= SI_AGENT_EVIDENCE_NONE ||
        item->kind > SI_AGENT_EVIDENCE_MODEL_ASSERTION ||
        item->outcome > SI_AGENT_EVIDENCE_OUTCOME_MISMATCH) {
        return false;
    }
    if (!bounded_nonempty(item->evidence_id, SI_AGENT_VERIFY_ID_MAX_LEN) ||
        !bounded_nonempty(item->action_id, SI_AGENT_VERIFY_ID_MAX_LEN) ||
        !bounded_nonempty(item->target_identity,
                          SI_AGENT_VERIFY_TARGET_MAX_LEN) ||
        !sha256_hex_valid(item->completion_criteria_hash) ||
        item->generation == 0U ||
        item->issuer <= SI_AGENT_EVIDENCE_ISSUER_UNSPECIFIED ||
        item->issuer > SI_AGENT_EVIDENCE_ISSUER_MODEL) {
        return false;
    }
    if (item->kind == SI_AGENT_EVIDENCE_ARTIFACT &&
        !sha256_hex_valid(item->artifact_hash)) {
        return false;
    }
    return true;
}

si_agent_verification_result_t si_agent_verify(
    const si_agent_verification_spec_t *spec,
    const si_agent_evidence_t *evidence,
    size_t evidence_count)
{
    si_agent_verification_result_t result = {
        .status = SI_AGENT_VERIFY_INVALID,
        .accepted_kind = SI_AGENT_EVIDENCE_NONE,
        .accepted_index = SIZE_MAX,
    };
    if (!spec || !bounded_nonempty(spec->action_id,
                                   SI_AGENT_VERIFY_ID_MAX_LEN) ||
        !bounded_nonempty(spec->target_identity,
                          SI_AGENT_VERIFY_TARGET_MAX_LEN) ||
        !sha256_hex_valid(spec->completion_criteria_hash) ||
        spec->minimum_generation == 0U ||
        !si_agent_evidence_issuer_is_authoritative(spec->required_issuer) ||
        spec->action_started_ms == 0U ||
        (evidence_count > 0U && !evidence)) {
        set_reason(&result, "invalid verification specification");
        return result;
    }
    if (evidence_count == 0U) {
        result.status = SI_AGENT_VERIFY_PENDING;
        set_reason(&result, "fresh device evidence required");
        return result;
    }

    bool saw_authoritative = false;
    bool saw_unknown = false;
    bool saw_invalid = false;
    for (size_t index = 0; index < evidence_count; ++index) {
        const si_agent_evidence_t *item = &evidence[index];
        if (!evidence_shape_valid(item)) {
            saw_invalid = true;
            continue;
        }
        if (!si_agent_evidence_kind_is_authoritative(item->kind) ||
            !si_agent_evidence_issuer_is_authoritative(item->issuer)) {
            continue;
        }
        if (spec->require_readback &&
            item->kind != SI_AGENT_EVIDENCE_READBACK) {
            continue;
        }
        if (!same_required(spec->action_id, item->action_id,
                           SI_AGENT_VERIFY_ID_MAX_LEN) ||
            !same_required(spec->target_identity, item->target_identity,
                           SI_AGENT_VERIFY_TARGET_MAX_LEN) ||
            !same_required(spec->completion_criteria_hash,
                           item->completion_criteria_hash,
                           SI_AGENT_VERIFY_HASH_MAX_LEN) ||
            item->issuer != spec->required_issuer ||
            item->generation < spec->minimum_generation ||
            item->captured_ms <= spec->action_started_ms) {
            continue;
        }
        saw_authoritative = true;
        if (item->outcome == SI_AGENT_EVIDENCE_OUTCOME_UNKNOWN) {
            saw_unknown = true;
            continue;
        }

        result.accepted_kind = item->kind;
        result.accepted_index = index;
        snprintf(result.evidence_id, sizeof(result.evidence_id), "%s",
                 item->evidence_id);
        if (item->outcome == SI_AGENT_EVIDENCE_OUTCOME_MISMATCH) {
            result.status = SI_AGENT_VERIFY_FAILED;
            set_reason(&result, "fresh device evidence contradicts expected effect");
            return result;
        }
        result.status = SI_AGENT_VERIFY_PASSED;
        set_reason(&result, "fresh device evidence matches expected effect");
        return result;
    }

    if (saw_unknown) {
        result.status = SI_AGENT_VERIFY_UNKNOWN;
        set_reason(&result, "device evidence outcome is unknown");
    } else if (saw_authoritative) {
        result.status = SI_AGENT_VERIFY_PENDING;
        set_reason(&result, "matching authoritative evidence required");
    } else if (saw_invalid) {
        result.status = SI_AGENT_VERIFY_INVALID;
        set_reason(&result, "malformed evidence rejected");
    } else {
        result.status = SI_AGENT_VERIFY_PENDING;
        set_reason(&result,
                   spec->mutation ?
                   "tool/model success is not mutation evidence" :
                   "fresh observation, artifact, or readback required");
    }
    return result;
}

const char *si_agent_evidence_kind_name(si_agent_evidence_kind_t kind)
{
    switch (kind) {
    case SI_AGENT_EVIDENCE_OBSERVATION:
        return "observation";
    case SI_AGENT_EVIDENCE_ARTIFACT:
        return "artifact";
    case SI_AGENT_EVIDENCE_READBACK:
        return "readback";
    case SI_AGENT_EVIDENCE_TOOL_RETURN:
        return "tool_return";
    case SI_AGENT_EVIDENCE_MODEL_ASSERTION:
        return "model_assertion";
    case SI_AGENT_EVIDENCE_NONE:
    default:
        return "none";
    }
}

const char *si_agent_verification_status_name(
    si_agent_verification_verdict_t status)
{
    switch (status) {
    case SI_AGENT_VERIFY_PENDING:
        return "pending";
    case SI_AGENT_VERIFY_PASSED:
        return "passed";
    case SI_AGENT_VERIFY_FAILED:
        return "failed";
    case SI_AGENT_VERIFY_UNKNOWN:
        return "unknown";
    case SI_AGENT_VERIFY_INVALID:
    default:
        return "invalid";
    }
}
