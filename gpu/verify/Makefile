# Makefile — CUDA verification programs for the GT 750M (Kepler sm_30, CUDA 10.2).
# Usage:
#   make           # builds all
#   make run       # builds + runs all
#   make clean

CUDA_HOME ?= /opt/cuda
NVCC      := $(CUDA_HOME)/bin/nvcc
ARCH      := -gencode arch=compute_30,code=sm_30
CCFLAGS   := $(ARCH) -O2 -std=c++14

PROGS := check_cuda vecadd bandwidth

all: $(PROGS)

%: %.cu
	$(NVCC) $(CCFLAGS) $< -o $@

run: all
	@echo "===== check_cuda ====="; ./check_cuda
	@echo; echo "===== vecadd ====="; ./vecadd
	@echo; echo "===== bandwidth ====="; ./bandwidth

clean:
	rm -f $(PROGS)

.PHONY: all run clean