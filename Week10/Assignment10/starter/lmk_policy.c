#include "../student_support/lmk_policy.h"
#include <errno.h>

/* Assignment tasks A/B. Fail explicitly instead of pretending to implement PSI. */
int lab_should_act(int amount_low, int psi_event) {
    if (psi_event) {
        errno = ENOSYS;
        return -1;
    }

    return amount_low;
}

int lab_choose(const lab_candidate *p, int n, int importance, int floor) {
    (void)floor;
    if (importance) {
        errno = ENOSYS;
        return -2;
    }

    int best = -1;
    for (int i = 0; i < n; i++)
        if (p[i].adj != -1000 && (best < 0 || p[i].score > p[best].score ||
                                  (p[i].score == p[best].score && p[i].rss_kib > p[best].rss_kib)))
            best = i;
    return best;
}
