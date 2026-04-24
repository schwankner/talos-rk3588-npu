/*
 * RKNN multi-threaded C API benchmark — RK3588 NPU
 *
 * Each thread owns its own rknn_context.  Contexts are initialised
 * SEQUENTIALLY in main() (rknn_init is not safe to call from multiple
 * threads simultaneously — concurrent calls crash rknpu.ko).  Each
 * context is pinned to a specific NPU core (thread_id % 3 → CORE_0/1/2)
 * so no two threads compete for the same hardware queue.
 *
 * After sequential init + warmup, a pthread_barrier releases all threads
 * simultaneously so the NPU scheduler sees N parallel job streams.
 * The RK3588 has 3 NPU cores; 3 threads (one per core) gives close to
 * 3× single-thread aggregate throughput.
 *
 * Design rules (hard-won):
 *   - rknn_init:          call only from main(), one at a time, NEVER from threads
 *   - RKNN_NPU_CORE_AUTO: forbidden for multi-context — deadlocks rknpu.ko driver
 *   - Explicit core masks: CORE_0/1/2 round-robin across threads
 *
 * Usage:
 *   bench_c_mt <model.rknn> <model_name> <threads> <iters_per_thread> [warmup]
 *   bench_c_mt /model/resnet18.rknn resnet18 3 1000 50
 *   bench_c_mt /model/yolov5s.rknn  yolov5s  3  200 20
 *
 * Compile (aarch64):
 *   gcc -O2 -o bench_c_mt bench_c_mt.c -I/path/to/include \
 *       -L/path/to/lib -lrknnrt -Wl,-rpath,/usr/lib -lpthread -lm
 */

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rknn_api.h"

/* -------------------------------------------------------------------------- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static uint8_t *load_model(const char *path, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: cannot open model %s\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *out_size = (size_t)ftell(fp);
    rewind(fp);
    uint8_t *buf = (uint8_t *)malloc(*out_size);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, *out_size, fp) != *out_size) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    return buf;
}

/* -------------------------------------------------------------------------- */

/* Barrier: all threads enter their timed loop simultaneously */
static pthread_barrier_t g_start_barrier;

typedef struct {
    int          thread_id;
    int          iters;
    /* Pre-initialised by main() before thread is spawned */
    rknn_context ctx;
    uint32_t     n_input;
    uint32_t     n_output;
    rknn_input   inp;
    uint8_t     *input_buf;
    rknn_output *outputs;
    /* Written by thread after completion */
    double       fps;
    double       ms_per;
    int          ok;
} thread_arg_t;

static void *bench_thread(void *arg)
{
    thread_arg_t *a = (thread_arg_t *)arg;
    a->ok = -1;

    /* Wait until main() releases all threads simultaneously */
    pthread_barrier_wait(&g_start_barrier);

    double t0 = now_ms();
    for (int i = 0; i < a->iters; i++) {
        rknn_inputs_set(a->ctx, a->n_input, &a->inp);
        rknn_run(a->ctx, NULL);
        rknn_outputs_get(a->ctx, a->n_output, a->outputs, NULL);
        rknn_outputs_release(a->ctx, a->n_output, a->outputs);
    }
    double elapsed = now_ms() - t0;

    a->fps   = (double)a->iters / (elapsed / 1000.0);
    a->ms_per = elapsed / (double)a->iters;
    a->ok    = 0;
    return NULL;
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "/model/resnet18.rknn";
    const char *model_name = argc > 2 ? argv[2] : "resnet18";
    int         n_threads  = argc > 3 ? atoi(argv[3]) : 3;
    int         iters      = argc > 4 ? atoi(argv[4]) : 1000;
    int         warmup_n   = argc > 5 ? atoi(argv[5]) : 50;

    if (n_threads < 1 || n_threads > 32) {
        fprintf(stderr, "ERROR: threads must be 1-32\n");
        return 1;
    }

    printf("=== RKNN C MT Benchmark  model=%s  threads=%d"
           "  iters/thread=%d  warmup=%d ===\n",
           model_name, n_threads, iters, warmup_n);

    size_t   model_size = 0;
    uint8_t *model_data = load_model(model_path, &model_size);
    if (!model_data) return 1;
    printf("  model=%s  size=%.1f MB\n",
           model_path, (double)model_size / (1024*1024));

    /* Core map: distribute threads across the 3 RK3588 NPU cores.
     * RKNN_NPU_CORE_AUTO must NOT be used here — concurrent contexts
     * with AUTO deadlock the rknpu.ko driver command queue. */
    static const rknn_core_mask core_map[3] = {
        RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2
    };

    thread_arg_t *args = (thread_arg_t *)calloc(n_threads, sizeof(thread_arg_t));
    pthread_t    *tids = (pthread_t *)calloc(n_threads, sizeof(pthread_t));

    /* -----------------------------------------------------------------------
     * Sequential initialisation — rknn_init is NOT thread-safe; calling it
     * from multiple threads concurrently crashes rknpu.ko.  All contexts are
     * set up here in the main thread before any bench thread is spawned.
     * ----------------------------------------------------------------------- */
    for (int t = 0; t < n_threads; t++) {
        args[t].thread_id = t;
        args[t].iters     = iters;

        int ret = rknn_init(&args[t].ctx, model_data, (uint32_t)model_size, 0, NULL);
        if (ret < 0) {
            fprintf(stderr, "ERROR: [T%d] rknn_init ret=%d\n", t, ret);
            return 1;
        }
        rknn_set_core_mask(args[t].ctx, core_map[t % 3]);

        if (t == 0) {
            rknn_sdk_version ver;
            memset(&ver, 0, sizeof(ver));
            rknn_query(args[0].ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
            printf("  SDK: %s  driver: %s\n", ver.api_version, ver.drv_version);
        }

        rknn_input_output_num io;
        memset(&io, 0, sizeof(io));
        rknn_query(args[t].ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
        args[t].n_input  = io.n_input;
        args[t].n_output = io.n_output;

        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = 0;
        rknn_query(args[t].ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr));

        size_t input_size = 1;
        for (uint32_t i = 0; i < attr.n_dims; i++)
            input_size *= attr.dims[i];

        args[t].input_buf = (uint8_t *)malloc(input_size);
        for (size_t i = 0; i < input_size; i++)
            args[t].input_buf[i] = (uint8_t)(i % 256);

        memset(&args[t].inp, 0, sizeof(args[t].inp));
        args[t].inp.index        = 0;
        args[t].inp.type         = RKNN_TENSOR_UINT8;
        args[t].inp.size         = (uint32_t)input_size;
        args[t].inp.fmt          = RKNN_TENSOR_NHWC;
        args[t].inp.buf          = args[t].input_buf;
        args[t].inp.pass_through = 0;

        args[t].outputs = (rknn_output *)calloc(io.n_output, sizeof(rknn_output));
        for (uint32_t i = 0; i < io.n_output; i++)
            args[t].outputs[i].want_float = 0;

        /* Warmup — sequential, prevents cold-start skew in the timed section */
        for (int w = 0; w < warmup_n; w++) {
            rknn_inputs_set(args[t].ctx, io.n_input, &args[t].inp);
            rknn_run(args[t].ctx, NULL);
            rknn_outputs_get(args[t].ctx, io.n_output, args[t].outputs, NULL);
            rknn_outputs_release(args[t].ctx, io.n_output, args[t].outputs);
        }
        printf("  [T%d] init+warmup done  core=CORE_%d\n", t, t % 3);
        fflush(stdout);

        /* Small inter-init delay — lets the NPU driver settle between contexts */
        if (t < n_threads - 1) usleep(20000);
    }

    free(model_data); /* rknn_init copied the model internally */

    printf("  All %d contexts ready — launching threads simultaneously...\n",
           n_threads);
    fflush(stdout);

    /* Barrier: n_threads workers + 1 (main releases all at once) */
    pthread_barrier_init(&g_start_barrier, NULL, (unsigned)(n_threads + 1));

    for (int t = 0; t < n_threads; t++)
        pthread_create(&tids[t], NULL, bench_thread, &args[t]);

    pthread_barrier_wait(&g_start_barrier); /* releases all threads */
    double wall_t0 = now_ms();
    printf("  All threads running — %d inferences each...\n", iters);
    fflush(stdout);

    for (int t = 0; t < n_threads; t++)
        pthread_join(tids[t], NULL);
    double wall_elapsed = now_ms() - wall_t0;

    pthread_barrier_destroy(&g_start_barrier);

    /* Print per-thread results, then aggregate */
    int    errors = 0;
    double avg_ms = 0.0;
    double max_ms = 0.0;

    for (int t = 0; t < n_threads; t++) {
        if (args[t].ok != 0) { errors++; continue; }
        printf("  [T%d] %.1f fps  %.2f ms/inf\n",
               t, args[t].fps, args[t].ms_per);
        avg_ms += args[t].ms_per;
        if (args[t].ms_per > max_ms) max_ms = args[t].ms_per;
    }
    if (n_threads - errors > 0)
        avg_ms /= (double)(n_threads - errors);

    /* Cleanup */
    for (int t = 0; t < n_threads; t++) {
        free(args[t].input_buf);
        free(args[t].outputs);
        rknn_destroy(args[t].ctx);
    }
    free(args);
    free(tids);

    if (errors > 0) {
        fprintf(stderr, "ERROR: %d/%d threads failed\n", errors, n_threads);
        return 1;
    }

    double wall_fps = (double)(n_threads * iters) / (wall_elapsed / 1000.0);

    printf("\n");
    printf("  Wall time  : %.3f s\n", wall_elapsed / 1000.0);
    printf("  Throughput : %.1f fps  (aggregate wall-clock)\n", wall_fps);
    printf("  Avg latency: %.2f ms / inference / thread\n", avg_ms);
    printf("  Max latency: %.2f ms / inference / thread\n", max_ms);
    printf("\n");
    printf("RESULT mode=npu-c-mt model=%s quant=INT8"
           " runtime='NPU C API MT (RK3588)' threads=%d"
           " fps=%.1f latency_ms=%.2f\n",
           model_name, n_threads, wall_fps, avg_ms);

    return 0;
}
