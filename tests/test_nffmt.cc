#include "nftest.h"
#include "nffmt.h"

// The measured bug, from the reference card on 2026-09-03:
// books/Comics/English/Sandman/ holds DIRECTORIES named v1, v10, v2 ... v9.
// A plain case-insensitive sort puts v10 -- The Wake, the finale -- second.
static void test_unpadded_volume_dirs(void) {
    CHECK(nf_natural_compare("v1 - Preludes and Nocturnes",
                             "v2 - The Doll's House") < 0);
    CHECK(nf_natural_compare("v2 - The Doll's House",
                             "v9 - The Kindly Ones") < 0);
    CHECK(nf_natural_compare("v9 - The Kindly Ones",
                             "v10 - The Wake") < 0);
}

int main(void) {
    test_unpadded_volume_dirs();
    NF_TEST_MAIN_END
}
