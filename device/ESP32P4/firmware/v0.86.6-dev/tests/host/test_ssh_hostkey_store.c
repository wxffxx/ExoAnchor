#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ssh_hostkey_store.h"

int main(void)
{
    const char *fingerprint =
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef";
    char changed[SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN + 1];
    memcpy(changed, fingerprint, sizeof(changed));

    assert(si_ssh_hostkey_record_valid(fingerprint));
    assert(si_ssh_hostkey_record_matches(fingerprint, fingerprint));

    changed[0] = '1';
    assert(!si_ssh_hostkey_record_matches(fingerprint, changed));
    memcpy(changed, fingerprint, sizeof(changed));
    changed[SI_SSH_HOSTKEY_FINGERPRINT_HEX_LEN - 1] = '0';
    assert(!si_ssh_hostkey_record_matches(fingerprint, changed));

    assert(!si_ssh_hostkey_record_valid(NULL));
    assert(!si_ssh_hostkey_record_valid("0123"));
    memcpy(changed, fingerprint, sizeof(changed));
    changed[17] = 'A';
    assert(!si_ssh_hostkey_record_valid(changed));
    assert(!si_ssh_hostkey_record_matches(changed, fingerprint));

    puts("ssh host-key policy tests passed");
    return 0;
}
