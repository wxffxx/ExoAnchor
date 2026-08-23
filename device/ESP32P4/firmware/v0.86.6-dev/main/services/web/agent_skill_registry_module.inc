// Merge newly added immutable firmware Skills into an older saved UI registry.
// Existing entries remain authoritative so user enable/disable choices survive.
static bool agent_skills_array_has_name(const cJSON *skills,
                                        const char *skill_name)
{
    if (!cJSON_IsArray(skills) || !skill_name) {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, skills) {
        const cJSON *name =
            cJSON_GetObjectItemCaseSensitive(item, "name");
        if (cJSON_IsString(name) && name->valuestring &&
            strcmp(name->valuestring, skill_name) == 0) {
            return true;
        }
    }
    return false;
}

static void agent_skills_merge_missing_defaults(cJSON *skills)
{
    if (!cJSON_IsArray(skills)) {
        return;
    }
    cJSON *defaults = cJSON_Parse(AGENT_TOOLS_SKILLS_DEFAULT);
    if (!cJSON_IsArray(defaults)) {
        cJSON_Delete(defaults);
        return;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, defaults) {
        const cJSON *name =
            cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *source =
            cJSON_GetObjectItemCaseSensitive(item, "source");
        if (!cJSON_IsString(name) || !name->valuestring ||
            !cJSON_IsString(source) || !source->valuestring ||
            strcmp(source->valuestring, "firmware") != 0 ||
            agent_skills_array_has_name(skills, name->valuestring)) {
            continue;
        }
        cJSON *copy = cJSON_Duplicate(item, true);
        if (copy) {
            cJSON_AddItemToArray(skills, copy);
        }
    }
    cJSON_Delete(defaults);
}

static cJSON *agent_skills_parse_with_defaults(char *skills,
                                               size_t skills_size)
{
    if (!skills || skills_size == 0) {
        return NULL;
    }
    cJSON *parsed = cJSON_Parse(skills);
    if (!cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        strlcpy(skills, AGENT_TOOLS_SKILLS_DEFAULT, skills_size);
        parsed = cJSON_Parse(skills);
    }
    if (!cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return NULL;
    }

    agent_skills_merge_missing_defaults(parsed);
    char *merged = cJSON_PrintUnformatted(parsed);
    if (merged && strlen(merged) < skills_size) {
        strlcpy(skills, merged, skills_size);
    }
    cJSON_free(merged);
    return parsed;
}
