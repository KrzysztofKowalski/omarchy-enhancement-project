// check_cuda.cu — query of CUDA device properties.
// Compile:  nvcc -gencode arch=compute_30,code=sm_30 check_cuda.cu -o check_cuda
// Run: ./check_cuda
//
// Checks: whether a CUDA device exists at all, which driver/runtime,
// the GPU name, compute capability (must be 3.0 for the GT 750M / Kepler) and memory.

#include <cstdio>
#include <cstdlib>

int main() {
    int devCount = 0;
    cudaError_t err = cudaGetDeviceCount(&devCount);
    if (err != cudaSuccess) {
        printf("FAIL: cudaGetDeviceCount -> %s\n", cudaGetErrorString(err));
        printf("      (is the nvidia kernel module not loaded? run: nvidia-smi)\n");
        return 2;
    }
    if (devCount == 0) {
        printf("FAIL: no CUDA devices (the nvidia driver is unavailable).\n");
        return 2;
    }

    int driverVersion = 0, runtimeVersion = 0;
    cudaDriverGetVersion(&driverVersion);
    cudaRuntimeGetVersion(&runtimeVersion);
    printf("NVIDIA API driver : %d.%d\n", driverVersion / 1000, (driverVersion % 100) / 10);
    printf("CUDA runtime      : %d.%d\n", runtimeVersion / 1000, (runtimeVersion % 100) / 10);

    for (int d = 0; d < devCount; ++d) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, d);
        printf("\n== Device %d ==\n", d);
        printf("  name             : %s\n", prop.name);
        printf("  compute capability: %d.%d\n", prop.major, prop.minor);
        printf("  global memory    : %zu MB (%llu bytes)\n",
               prop.totalGlobalMem / (1024 * 1024),
               (unsigned long long)prop.totalGlobalMem);
        printf("  multiprocessors  : %d\n", prop.multiProcessorCount);
        printf("  shared/block     : %zu KB\n", prop.sharedMemPerBlock / 1024);
        printf("  warp size        : %d\n", prop.warpSize);
        printf("  max block (1D)   : %d\n", prop.maxThreadsDim[0]);

        if (prop.major == 3 && prop.minor == 0) {
            printf("  -> OK: this is Kepler sm_30, supported by CUDA 10.2.\n");
        } else {
            printf("  -> WARNING: compute %d.%d — this is not sm_30 (expected for the GT 750M).\n",
                   prop.major, prop.minor);
        }
    }

    printf("\nPASS: the CUDA runtime sees a device.\n");
    return 0;
}