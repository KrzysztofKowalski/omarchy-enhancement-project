// bandwidth.cu — GPU memory bandwidth test (memcpy H<->D).
// Compile:  nvcc -gencode arch=compute_30,code=sm_30 bandwidth.cu -o bandwidth
// Run: ./bandwidth [MB]
//
// Gives a realistic picture of whether the card actually transfers data
// (and does not run through, e.g., a software fallback). GT 750M ~10-28 GB/s.

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    size_t MB = (argc > 1) ? (size_t)atoi(argv[1]) : 64;
    size_t bytes = MB * 1024 * 1024;
    printf("Buffer size: %zu MB\n", MB);

    void *h = malloc(bytes);
    void *h2 = malloc(bytes);
    memset(h, 0x55, bytes);
    if (!h || !h2) { printf("host malloc failed\n"); return 2; }

    void *d = nullptr;
    cudaError_t err = cudaMalloc(&d, bytes);
    if (err != cudaSuccess) { printf("cudaMalloc: %s\n", cudaGetErrorString(err)); return 2; }

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);

    const int iters = 10;

    // H -> D
    cudaEventRecord(t0);
    for (int i = 0; i < iters; ++i) cudaMemcpy(d, h, bytes, cudaMemcpyHostToDevice);
    cudaEventRecord(t1); cudaEventSynchronize(t1);
    float msH2D = 0; cudaEventElapsedTime(&msH2D, t0, t1);
    double gbH2D = (double)bytes * iters / (msH2D / 1000.0) / 1e9;

    // D -> H
    cudaEventRecord(t0);
    for (int i = 0; i < iters; ++i) cudaMemcpy(h2, d, bytes, cudaMemcpyDeviceToHost);
    cudaEventRecord(t1); cudaEventSynchronize(t1);
    float msD2H = 0; cudaEventElapsedTime(&msD2H, t0, t1);
    double gbD2H = (double)bytes * iters / (msD2H / 1000.0) / 1e9;

    printf("H->D: %.2f GB/s  (%.2f ms/transfer)\n", gbH2D, msH2D / iters);
    printf("D->H: %.2f GB/s  (%.2f ms/transfer)\n", gbD2H, msD2H / iters);

    cudaFree(d); free(h); free(h2);
    cudaEventDestroy(t0); cudaEventDestroy(t1);
    return 0;
}