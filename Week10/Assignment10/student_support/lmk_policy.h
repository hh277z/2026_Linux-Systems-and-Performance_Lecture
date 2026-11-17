#ifndef LAB10_POLICY_H
#define LAB10_POLICY_H

typedef struct {
    int pid, adj, score;
    long rss_kib;
} lab_candidate;

/* pressure is a fresh interval decision, not PSI avg10 */
int lab_should_act(int amount_low, int psi_event);
int lab_choose(const lab_candidate *p, int n, int importance, int floor);
#endif
