#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "agent_api_settings.h"

static void expect_valid(const char *endpoint)
{
    assert(si_agent_api_validate_endpoint(endpoint, false) == ESP_OK);
}

static void expect_invalid(const char *endpoint)
{
    assert(si_agent_api_validate_endpoint(endpoint, false) != ESP_OK);
}

int main(void)
{
    expect_valid("https://api.deepseek.com");
    expect_valid("https://api.openai.com/v1");
    expect_valid("https://dashscope.aliyuncs.com/compatible-mode/v1");
    expect_valid("https://api.moonshot.cn/v1");
    expect_valid("https://example.com:443/v1");
    expect_valid("https://[2001:db8::1]:8443/v1");

    assert(si_agent_api_validate_endpoint("", true) == ESP_OK);
    expect_invalid("");
    expect_invalid("http://api.deepseek.com");
    expect_invalid("https://");
    expect_invalid("https://user@example.com/v1");
    expect_invalid("https://example.com/a b");
    expect_invalid("https://example.com\\v1");
    expect_invalid("https://example.com/v1?tenant=other");
    expect_invalid("https://example.com/v1#fragment");
    expect_invalid("https://example.com:/v1");
    expect_invalid("https://example.com:0/v1");
    expect_invalid("https://example.com:65536/v1");
    expect_invalid("https://example.com:443x/v1");
    expect_invalid("https://2001:db8::1/v1");

    puts("agent endpoint validation tests: PASS");
    return 0;
}
