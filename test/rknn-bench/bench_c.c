/*
 * RKNN C API benchmark — RK3588 NPU
 *
 * Uses rknn_init / rknn_inputs_set / rknn_run / rknn_outputs_get directly,
 * eliminating the 3-5 ms Python/rknnlite API overhead per call and showing
 * the true NPU throughput that matches the published 10-12× speedup figures.
 *
 * Usage:
 *   bench_c <model.rknn> <model_name> <iterations> [warmup]
 *   bench_c /model/resnet18.rknn resnet18 2000 50
 *   bench_c /model/yolov5s.rknn  yolov5s  500  20
 *
 * Compile (aarch64):
 *   gcc -O2 -o bench_c bench_c.c -I/path/to/include \
 *       -L/path/to/lib -lrknnrt -Wl,-rpath,/usr/lib -lpthread -lm
 */

#include <math.h>
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
        fprintf(stderr, "ERROR: malloc %zu bytes\n", *out_size);
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, *out_size, fp) != *out_size) {
        fprintf(stderr, "ERROR: read %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

/* -------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "/model/resnet18.rknn";
    const char *model_name = argc > 2 ? argv[2] : "resnet18";
    int         iterations = argc > 3 ? atoi(argv[3]) : 2000;
    int         warmup     = argc > 4 ? atoi(argv[4]) : 50;

    printf("=== RKNN C API Benchmark  model=%s  iterations=%d  warmup=%d ===\n",
           model_name, iterations, warmup);

    /* Load model file */
    size_t   model_size = 0;
    uint8_t *model_data = load_model(model_path, &model_size);
    if (!model_data) return 1;
    printf("  model=%s  size=%.1f MB\n", model_path, (double)model_size / (1024*1024));

    /* Initialise RKNN context */
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_data, (uint32_t)model_size, 0, NULL);
    free(model_data);
    if (ret < 0) {
        fprintf(stderr, "ERROR: rknn_init ret=%d\n", ret);
        return 1;
    }

    /* SDK / driver versions */
    rknn_sdk_version sdk_ver;
    memset(&sdk_ver, 0, sizeof(sdk_ver));
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver));
    printf("  SDK: %s  driver: %s\n", sdk_ver.api_version, sdk_ver.drv_version);

    /* NPU core assignment — AUTO lets the runtime pick the best available */
    rknn_set_core_mask(ctx, RKNN_NPU_CORE_AUTO);

    /* Query input / output counts */
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    printf("  inputs=%u  outputs=%u\n", io_num.n_input, io_num.n_output);

    /* Query first input tensor attributes */
    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    printf("  input[0]: name=%s  type=%d  fmt=%d\n",
           input_attr.name, input_attr.type, input_attr.fmt);

    /* Compute input buffer size from tensor dims */
    size_t input_size = 1;
    for (uint32_t i = 0; i < input_attr.n_dims; i++)
        input_size *= input_attr.dims[i];
    printf("  input size = %zu bytes\n", input_size);

    /* Allocate and fill with a fixed pattern — latency test, not accuracy */
    uint8_t *input_buf = (uint8_t *)malloc(input_size);
    if (!input_buf) { fprintf(stderr, "ERROR: malloc input\n"); rknn_destroy(ctx); return 1; }
    for (size_t i = 0; i < input_size; i++)
        input_buf[i] = (uint8_t)(i % 256);

    /* Set up input descriptor */
    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index        = 0;
    input.type         = RKNN_TENSOR_UINT8;
    input.size         = (uint32_t)input_size;
    input.fmt          = RKNN_TENSOR_NHWC;
    input.buf          = input_buf;
    input.pass_through = 0;   /* let runtime do normalisation from raw uint8 */

    /* Output descriptors — raw quantised values (no float conversion overhead) */
    rknn_output *outputs = (rknn_output *)calloc(io_num.n_output, sizeof(rknn_output));
    if (!outputs) { free(input_buf); rknn_destroy(ctx); return 1; }
    for (uint32_t i = 0; i < io_num.n_output; i++)
        outputs[i].want_float = 0;

    /* ---------- Warmup ---------- */
    printf("  Warmup (%d iters)...\n", warmup);
    fflush(stdout);
    for (int i = 0; i < warmup; i++) {
        rknn_inputs_set(ctx, io_num.n_input, &input);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }

    /* ---------- Benchmark ---------- */
    printf("  Running %d inferences...\n", iterations);
    fflush(stdout);

    double t0 = now_ms();
    for (int i = 0; i < iterations; i++) {
        rknn_inputs_set(ctx, io_num.n_input, &input);
        rknn_run(ctx, NULL);
        rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
        rknn_outputs_release(ctx, io_num.n_output, outputs);
    }
    double elapsed_ms = now_ms() - t0;

    free(input_buf);
    free(outputs);
    rknn_destroy(ctx);

    double fps    = (double)iterations / (elapsed_ms / 1000.0);
    double ms_per = elapsed_ms / (double)iterations;

    printf("\n");
    printf("  Total time : %.3f s\n", elapsed_ms / 1000.0);
    printf("  Throughput : %.1f fps\n", fps);
    printf("  Latency    : %.2f ms / inference\n", ms_per);
    printf("\n");
    printf("RESULT mode=npu-c model=%s quant=INT8 runtime='NPU C API (RK3588)'"
           " fps=%.1f latency_ms=%.2f\n",
           model_name, fps, ms_per);

    return 0;
}
