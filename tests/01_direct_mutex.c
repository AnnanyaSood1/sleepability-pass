/* 01 - Direct leaf primitive via a rust_helper_* shim.
 * Mirrors Rust-for-Linux's rust/helpers/*.c wrappers, which forward straight
 * to a blocking C primitive. Expected: rust_helper_mutex_lock MAY SLEEP. */
extern void mutex_lock(void *lock);

void rust_helper_mutex_lock(void *lock) {
    mutex_lock(lock);
}
