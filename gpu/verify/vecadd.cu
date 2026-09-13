// vecadd.cu — vector addition on the GPU (verification that the kernel actually works).
// Compile:  nvcc -gencode arch=compute_30,code=sm_30 vecadd.cu -o vecadd
// Run: ./vecadd [N]
//
// Picks two random vectors, adds them on the GPU (kernel), compares with the CPU result.
// PASS = the kernel ran on the card and the result is correct.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

__global__ void vecAdd(const float *a, const float *b, float *c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) c[i] = a[i] + b[i];
}

int main(int argc, char **argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 1 << 20;  // default 1M elements
    if (N <= 0) N = 1 << 20;
    printf("N = %d elements (%.1f MB/vector)\n", N, (float)N * sizeof(float) / (1024 * 1024));

    std::vector<float> hA(N), hB(N), hC(N);
    for (int i = 0; i < N; ++i) {
        hA[i] = (float)(rand() % 1000) / 10.0f;
        hB[i] = (float)(rand() % 1000) / 10.0f;
    }

    // allocation on the GPU
    float *dA = nullptr, *dB = nullptr, *dC = nullptr;
    cudaError_t err;
    err = cudaMalloc(&dA, N * sizeof(float)); if (err) { printf("cudaMalloc A: %s\n", cudaGetErrorString(err)); return 2; }
    err = cudaMalloc(&dB, N * sizeof(float)); if (err) { printf("cudaMalloc B: %s\n", cudaGetErrorString(err)); return 2; }
    err = cudaMalloc(&dC, N * sizeof(float)); if (err) { printf("cudaMalloc C: %s\n", cudaGetErrorString(err)); return 2; }

    // H->D copies
    err = cudaMemcpy(dA, hA.data(), N * sizeof(float), cudaMemcpyHostToDevice);
    if (err) { printf("memcpy H->D A: %s\n", cudaGetErrorString(err)); return 2; }
    cudaMemcpy(dB, hB.data(), N * sizeof(float), cudaMemcpyHostToDevice);

    // launch kernel
    int threads = 256;
    int blocks = (N + threads - 1) / threads;
    printf("kernel: grid=%d block=%d (threads=%d)\n", blocks, threads, blocks * threads);

    cudaEvent_t t0, t1;
    cudaEventCreate(&t0); cudaEventCreate(&t1);
    cudaEventRecord(t0);
    vecAdd<<<blocks, threads>>>(dA, dB, dC, N);
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("FAIL: kernel launch -> %s\n", cudaGetErrorString(err));
        return 3;
    }

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, t0, t1);

    // D->H copy
    err = cudaMemcpy(hC.data(), dC, N * sizeof(float), cudaMemcpyDeviceToHost);
    if (err) { printf("memcpy D->H C: %s\n", cudaGetErrorString(err)); return 2; }

    // verification
    int mismatches = 0;
    float maxErr = 0.0f;
    for (int i = 0; i < N; ++i) {
        float expect = hA[i] + hB[i];
        float diff = fabsf(hC[i] - expect);
        if (diff > maxErr) maxErr = diff;
        if (diff > 1e-4f) mismatches++;
    }

    printf("kernel time  : %.3f ms\n", ms);
    printf("max error    : %g\n", maxErr);
    printf("mismatches   : %d / %d\n", mismatches, N);

    cudaFree(dA); cudaFree(dB); cudaFree(dC);
    cudaEventDestroy(t0); cudaEventDestroy(t1);

    if (mismatches == 0) {
        printf("PASS: the kernel ran on the GPU, the result is correct.\n");
        return 0;
    } else {
        printf("FAIL: wrong results (%d differences).\n", mismatches);
        return 1;
    }
}