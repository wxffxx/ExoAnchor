# cJSON for host tests

These unmodified files come from cJSON commit
[`b2890c8d76bbb64e710585ebc0a917196b9c67e7`](https://github.com/DaveGamble/cJSON/tree/b2890c8d76bbb64e710585ebc0a917196b9c67e7),
the submodule revision pinned by
[ESP-IDF v5.5.5](https://github.com/espressif/esp-idf/tree/v5.5.5/components/json).
The bundled [MIT license](LICENSE) and source notices apply.

Host tests compile this copy directly, so they do not require ESP-IDF or
download dependencies. Firmware builds continue to use ESP-IDF's JSON
component; this test copy is not added to the firmware component sources.

| File | SHA-256 |
| --- | --- |
| cJSON.c | `607e756460fa0de37d20a7a9181f2de29c97bfb7ce5a0e6c2f548243836cd852` |
| cJSON.h | `25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee` |
| LICENSE | `a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c` |

When the firmware's ESP-IDF baseline changes, check its cJSON submodule
revision, update all three upstream files and these hashes together, and run
the complete host test suite. Do not patch this copy independently of the
firmware dependency.
