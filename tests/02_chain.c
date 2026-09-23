/* 02 - Transitivity across three levels: level3 -> level2 -> level1 -> schedule.
 * All three must be MAY SLEEP; tests reachability, not just direct calls. */
extern void schedule(void);

static void level1(void) { schedule(); }
static void level2(void) { level1(); }
void        level3(void) { level2(); }   /* external entry reaches the chain */
