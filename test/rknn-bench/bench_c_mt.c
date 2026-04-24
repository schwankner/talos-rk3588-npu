/*
 * RKNN multi-threaded C API benchmark — RK3588 NPU
 *
 * Each thread owns its own rknn_context (RKNN context is not thread-safe for
 * concurrent use — every thread must call rknn_init separately).  All threads
 * start their benchmark loop simultaneously via a pthread barrier, so the NPU
 * scheduler sees N independent job streams in parallel.
 *
 * This reflects real inference-server usage: N concurrent requests arriving
 * at the same time, each dispatched to the NPU independently.  The RK3588
 * NPU has 3 cores; with 3 threads using NPU_CORE_AUTO the runtime pins each
 * context to a separate core, giving close to 3× single-thread throughput.
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
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, *out_size, fp) != *out_size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

/* -------------------------------------------------------------------------- */

/* Shared model data — loaded once, each thread calls rknn_init with its own ctx */
static const uint8_t *g_model_data = NULL;
static uint32_t       g_model_size = 0;
static int            g_warmup     = 50;
static int            g_iters      = 1000;

/* Barrier: all threads start their benchmark loop at the same instant */
static pthread_barrier_t g_start_barrier;

typedef struct {
    int    thread_id;
    double fps;        /* result */
    double ms_per;     /* result */
    double elapsed_ms; /* result */
    int    ok;         /* 0 = success, -1 = error */
} thread_result_t;

static void *bench_thread(void *arg)
{
    thread_result_t *res = (thread_result_t *)arg;
    res->ok = -1;

    /* Each thread creates its own context from the shared model buffer */
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, (void *)g_model_data, g_model_size, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "  [T%d] rknn_init failed ret=%d\n", res->thread_id, ret);
        return NULL;
    }

    rknn_set_core_mask(ctx, RKNN_NPU_CORE_AUTO);

    /* Query I/O counts and first input shape */
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));

    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));

    size_t input_size = 1;
    for (uint32_t i = 0; i < input_attr.n_dims; i++)
        input_size *= input_attr.dims[i];

    uint8_t *input_buf = (uint8_t *)malloc(input_size);
    if (!input_buf) {
        rknn_destroy(ctx);
        return NULL;
    }
    for (size_t i = 0; i < input_size; i++)
        input_buf[i] = (uint8_t)(i % 256);

    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index        = 0;
    input.type         = RKNN_TENSOR_UINT8;
    input.size         = (uint32_t)input_size;
    input.fmt          = RKNN_TENSOR_NHWC;
    input.buf          = input_buf;
    input.pass_through = 0;

    rknn_output *outputs = (rknn_output *)calloc(io_num.n_output, sizeof(rknn_output));
    if (!outputs) {
        free(input_buf);
        rknn_destroy(ctx);
        return NULL;
    }
    for (uint32_t i = 0; i < io_num.n_output; i++)
        outputs[i].want_float = 0;

    /* Warmup — not timed, not synchronised */
    for (int i = 0; i < g_warmup; i++) {
        rknn_inputs_set(ctx, io_num.n_input, &input);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }

    /* All threads reach the barrier together before starting the timed section */
    pthread_barrier_wait(&g_start_barrier);

    double t0 = now_ms();
    for (int i = 0; i < g_iters; i++) {
        rknn_inputs_set(ctx, io_num.n_input, &input);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }
    double elapsed = now_ms() - t0;

    free(input_buf);
    free(outputs);
    rknn_destroy(ctx);

    res->elapsed_ms = elapsed;
    res->fps        = (double)g_iters / (elapsed / 1000.0);
    res->ms_per     = elapsed / (double)g_iters;
    res->ok         = 0;
    return NULL;
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "/model/resnet18.rknn";
    const char *model_name = argc > 2 ? argv[2] : "resnet18";
    int         n_threads  = argc > 3 ? atoi(argv[3]) : 3;
    g_iters                = argc > 4 ? atoi(argv[4]) : 1000;
    g_warmup               = argc > 5 ? atoi(argv[5]) : 50;

    if (n_threads < 1 || n_threads > 32) {
        fprintf(stderr, "ERROR: threads must be 1-32\n");
        return 1;
    }

    printf("=== RKNN C MT Benchmark  model=%s  threads=%d"
           "  iters/thread=%d  warmup=%d ===\n",
           model_name, n_threads, g_iters, g_warmup);

    /* Load model file once — all threads share the same read-only buffer */
    size_t model_size_sz = 0;
    uint8_t *model_data = load_model(model_path, &model_size_sz);
    if (!model_data) return 1;
    printf("  model=%s  size=%.1f MB\n",
           model_path, (double)model_size_sz / (1024*1024));

    g_model_data = model_data;
    g_model_size = (uint32_t)model_size_sz;

    /* Query SDK version from a temporary context */
    rknn_context tmp_ctx = 0;
    rknn_init(&tmp_ctx, model_data, g_model_size, 0, NULL);
    rknn_sdk_version sdk_ver;
    memset(&sdk_ver, 0, sizeof(sdk_ver));
    rknn_query(tmp_ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
    printf("  SDK: %s  driver: %s\n", sdk_ver.api_version, sdk_ver.drv_version);
    rknn_destroy(tmp_ctx);

    /* Barrier: n_threads workers + 1 (main, to release them all) */
    pthread_barrier_init(&g_start_barrier, NULL, (unsigned)(n_threads + 1));

    thread_result_t *results = (thread_result_t *)calloc(n_threads, sizeof(thread_result_t));
    pthread_t       *threads = (pthread_t *)calloc(n_threads, sizeof(pthread_t));

    printf("  Launching %d threads (warmup=%d, then synchronised start)...\n",
           n_threads, g_warmup);
    fflush(stdout);

    for (int t = 0; t < n_threads; t++) {
        results[t].thread_id = t;
        pthread_create(&threads[t], NULL, bench_thread, &results[t]);
    }

    /* Wait for all threads to finish warmup, then release them simultaneously */
    pthread_barrier_wait(&g_start_barrier);
    double wall_t0 = now_ms();
    printf("  All threads started — running %d inferences each...\n",
           g_iters);
    fflush(stdout);

    for (int t = 0; t < n_threads; t++)
        pthread_join(threads[t], NULL);
    double wall_elapsed = now_ms() - wall_t0;

    free(model_data);
    pthread_barrier_destroy(&g_start_barrier);

    /* Aggregate results */
    int    errors        = 0;
    double total_fps     = 0.0;
    double avg_ms        = 0.0;
    double max_ms        = 0.0;

    for (int t = 0; t < n_threads; t++) {
        if (results[t].ok != 0) { errors++; continue; }
        total_fps += results[t].fps;
        avg_ms    += results[t].ms_per;
        if (results[t].ms_per > max_ms) max_ms = results[t].ms_per;
        printf("  [T%d] %.1f fps  %.2f ms/inf\n",
               t, results[t].fps, results[t].ms_per);
    }
    if (n_threads - errors > 0)
        avg_ms /= (double)(n_threads - errors);

    free(results);
    free(threads);

    if (errors > 0) {
        fprintf(stderr, "ERROR: %d/%d threads failed\n", errors, n_threads);
        return 1;
    }

    double wall_fps = (double)(n_threads * g_iters) / (wall_elapsed / 1000.0);

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
