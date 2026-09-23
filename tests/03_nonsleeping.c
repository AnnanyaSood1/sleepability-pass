/* 03 - Genuinely non-sleeping code. No path to any blocking primitive.
 * Everything here must stay ATOMIC-SAFE. */
extern void *memcpy(void *, const void *, unsigned long);

int add(int a, int b) { return a + b; }

int sum_array(const int *xs, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += xs[i];
    return s;
}

void copy_thing(void *d, const void *s, unsigned long n) {
    memcpy(d, s, n);            /* memcpy is not a blocking primitive */
}
