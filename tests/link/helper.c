/* Cross-TU part 2: the helper, defined separately. It reaches mutex_lock. */
extern void mutex_lock(void *lock);

void acquire_lock(void) {
    static long the_lock;
    mutex_lock(&the_lock);
}
