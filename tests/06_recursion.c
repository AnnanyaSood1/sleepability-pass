/* 06 - SCC handling. Cycles in the call graph must collapse to one verdict.
 * ping/pong are mutually recursive and one branch reaches might_sleep(): both
 * MAY SLEEP. even/odd are mutually recursive with no blocking primitive: both
 * ATOMIC-SAFE. */
extern void might_sleep(void);
extern int  cond(void);

void ping(int n);
void pong(int n);
void ping(int n) { if (n <= 0) { might_sleep(); return; } pong(n - 1); }
void pong(int n) { if (cond()) ping(n - 1); }

unsigned even(unsigned n);
unsigned odd(unsigned n);
unsigned even(unsigned n) { return n == 0 ? 1u : odd(n - 1);  }
unsigned odd(unsigned n)  { return n == 0 ? 0u : even(n - 1); }
