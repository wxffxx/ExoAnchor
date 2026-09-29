#pragma once

#include <stdbool.h>

typedef struct {
    bool text_input;
    bool image_input;
    bool video_input;
    bool base64_image_input;
    bool public_image_url;
    bool native_tools;
    bool structured_json;
    bool streaming;
    bool reasoning;
    bool preserve_assistant_message;
    bool fixed_sampling_parameters;
    const char *default_reasoning_effort;
} si_model_capabilities_t;

void si_model_capabilities_resolve(const char *provider, const char *model,
                                   si_model_capabilities_t *capabilities);
bool si_model_is_kimi_k3(const char *provider, const char *model);
