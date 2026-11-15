/* 08 - Indirect calls are UNKNOWN, never silently safe.
 * A call through a function pointer cannot be resolved by this pass, so the
 * function is reported UNKNOWN (the consumer treats it as possibly sleeping).
 * A function that also sleeps directly stays MAY SLEEP, the stronger verdict.
 *
 *   via_ptr        UNKNOWN
 *   calls_via_ptr  UNKNOWN (inherits from its callee)
 *   both           MAY SLEEP
 */
extern void mutex_lock(void *lock);
typedef void (*lock_fn)(void *);

void via_ptr(lock_fn f, void *l) { f(l); }
void calls_via_ptr(void *l) { via_ptr(mutex_lock, l); }
void both(lock_fn f, void *l) { f(l); mutex_lock(l); }
