#ifndef ATM_BENCH_H
#define ATM_BENCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Benchmark results ─── */
typedef struct {
    const char *name;
    double time_ms;
    double throughput;  /* glyphs/sec */
    int glyphs_processed;
    int cache_hits;
    int cache_misses;
    size_t memory_used;
} atm_bench_result_t;

/* ─── Run full benchmark suite ─── */
void atm_run_benchmarks(int iterations);

/* ─── Render a simple glyph outline for benchmarking ───
 * Creates a synthetic 'A' glyph outline for testing. */
int atm_bench_make_test_glyph(atm_glyph_outline_t *outline,
                              const char *shape);

#ifdef __cplusplus
}
#endif

#endif /* ATM_BENCH_H */
