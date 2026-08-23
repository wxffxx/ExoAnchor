#include "version_utils.h"

#include <ctype.h>
#include <stdlib.h>

int si_version_compare(const char *a, const char *b)
{
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    if (*a == 'v' || *a == 'V') {
        a++;
    }
    if (*b == 'v' || *b == 'V') {
        b++;
    }

    const char *pa = a;
    const char *pb = b;
    while (*pa || *pb) {
        while (*pa && !isalnum((unsigned char)*pa)) {
            pa++;
        }
        while (*pb && !isalnum((unsigned char)*pb)) {
            pb++;
        }
        if (isdigit((unsigned char)*pa) && isdigit((unsigned char)*pb)) {
            unsigned long va = strtoul(pa, (char **)&pa, 10);
            unsigned long vb = strtoul(pb, (char **)&pb, 10);
            if (va != vb) {
                return va > vb ? 1 : -1;
            }
            continue;
        }
        int ca = tolower((unsigned char)*pa);
        int cb = tolower((unsigned char)*pb);
        if (ca != cb) {
            return ca > cb ? 1 : -1;
        }
        if (*pa) {
            pa++;
        }
        if (*pb) {
            pb++;
        }
    }
    return 0;
}
