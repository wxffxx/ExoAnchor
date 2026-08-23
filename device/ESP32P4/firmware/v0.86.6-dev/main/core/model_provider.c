#include "model_provider.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool text_equal_ci(const char *left, const char *right)
{
    if (!left || !right) {
        return false;
    }
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

static bool text_starts_with_ci(const char *value, const char *prefix)
{
    if (!value || !prefix) {
        return false;
    }
    while (*prefix) {
        if (!*value ||
            tolower((unsigned char)*value) !=
                tolower((unsigned char)*prefix)) {
            return false;
        }
        value++;
        prefix++;
    }
    return true;
}

static bool text_contains_ci(const char *value, const char *needle)
{
    if (!value || !needle || !needle[0]) {
        return false;
    }
    for (; *value; value++) {
        if (text_starts_with_ci(value, needle)) {
            return true;
        }
    }
    return false;
}

bool si_model_is_kimi_k3(const char *provider, const char *model)
{
    return text_equal_ci(provider, "kimi") &&
           text_starts_with_ci(model, "kimi-k3");
}

void si_model_capabilities_resolve(const char *provider, const char *model,
                                   si_model_capabilities_t *capabilities)
{
    if (!capabilities) {
        return;
    }

    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->text_input = true;

    if (text_equal_ci(provider, "deepseek")) {
        capabilities->native_tools = true;
        capabilities->structured_json = true;
        capabilities->streaming = true;
        capabilities->reasoning = true;
        return;
    }

    if (text_equal_ci(provider, "openai")) {
        capabilities->image_input = true;
        capabilities->base64_image_input = true;
        capabilities->public_image_url = true;
        capabilities->native_tools = true;
        capabilities->structured_json = true;
        capabilities->streaming = true;
        capabilities->reasoning = true;
        return;
    }

    if (text_equal_ci(provider, "qwen")) {
        bool vision_model = text_contains_ci(model, "-vl") ||
                            text_contains_ci(model, "omni");
        capabilities->image_input = vision_model;
        capabilities->video_input = vision_model;
        capabilities->base64_image_input = vision_model;
        capabilities->public_image_url = vision_model;
        capabilities->native_tools = true;
        capabilities->structured_json = true;
        capabilities->streaming = true;
        capabilities->reasoning = true;
        return;
    }

    if (si_model_is_kimi_k3(provider, model)) {
        capabilities->image_input = true;
        capabilities->video_input = true;
        capabilities->base64_image_input = true;
        capabilities->native_tools = true;
        capabilities->structured_json = true;
        capabilities->streaming = true;
        capabilities->reasoning = true;
        capabilities->preserve_assistant_message = true;
        capabilities->fixed_sampling_parameters = true;
        capabilities->default_reasoning_effort = "low";
        return;
    }

    if (text_equal_ci(provider, "kimi")) {
        capabilities->native_tools = true;
        capabilities->structured_json = true;
        capabilities->streaming = true;
    }
}
