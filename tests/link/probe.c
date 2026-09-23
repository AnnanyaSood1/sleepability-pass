/* Cross-TU part 1: the "driver". device_probe calls a helper that lives in a
 * different translation unit (helper.c). After linking, device_probe must be
 * MAY SLEEP even though nothing blocking appears in this file. */
extern void acquire_lock(void);

void device_probe(void) {
    acquire_lock();
}
