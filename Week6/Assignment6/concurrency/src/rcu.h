/*
 * rcu.h - user-space RCU, quiescent-state based (QSBR), as in Lab 6 cfgread
 * (provided, do not modify).
 *
 * Readers take no lock. Every thread of devsvc is registered, and the driver
 * reports a quiescent state after every request, outside all locks. Between
 * two quiescent states a thread may use any pointer it got from
 * rcu_dereference.
 *
 * A writer publishes a new object with rcu_assign_pointer, then calls
 * synchronize_rcu. It returns after every other registered thread has passed
 * a quiescent state, so no reader can still hold the old pointer, and only
 * then may the writer free the old object.
 *
 * synchronize_rcu waits (it polls with usleep). A thread that is blocked, on a
 * lock for example, does not pass a quiescent state. Call it only outside a
 * read-side section: the calling thread must not use RCU pointers it loaded
 * before the call.
 */
#ifndef RCU_H
#define RCU_H

void rcu_register_thread(void);
void rcu_unregister_thread(void);
void rcu_quiescent_state(void);
void synchronize_rcu(void);

/* Load a pointer that a writer may replace at any time. Load it once and use
 * that value: two loads may return two different objects. */
#define rcu_dereference(p)        __atomic_load_n(&(p), __ATOMIC_ACQUIRE)

/* Publish v as the new value of p. Fill the object completely before. */
#define rcu_assign_pointer(p, v)  __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

#endif
