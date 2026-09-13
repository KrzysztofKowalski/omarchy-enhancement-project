/* Benchmark: scalar vs AVX2/FMA NLMS kernel on synthetic speech/room echo.
 *
 * Builds a fake scenario:
 *   far  = "music" (sum of sinusoids + noise)
 *   near = far delayed 200ms, convolved with a 200-tap decaying room IR,
 *          plus speech bursts and a small noise floor
 * Runs 10s of 48kHz stereo through both kernels and reports CPU cost
 * (% of one core) and echo rejection (ERLE) measured in the far-only
 * segments of the last 2 seconds.
 */
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "aecn.h"

#define RATE 48000
#define SECS 10
#define N (RATE * SECS)
#define IR_LEN 200
#define DELAY (200 * RATE / 1000)

static float ir[IR_LEN], far_sig[N], near_sig[N];

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void build_signals(unsigned seed)
{
	unsigned r = seed;
	for (int i = 0; i < N; i++) {
		r = r * 1664525u + 1013904223u;
		float n = (int) r / 2147483648.0f - 0.5f;
		float t = (float) i / RATE;
		far_sig[i] = 0.2f * n + 0.15f * sinf(2 * 3.14159f * 220.0f * t)
			   + 0.10f * sinf(2 * 3.14159f * 440.3f * t)
			   + 0.07f * sinf(2 * 3.14159f * 587.7f * t);
	}
	srand(seed);
	float e = 0.f;
	for (int i = 0; i < IR_LEN; i++) {
		ir[i] = (rand() / (float) RAND_MAX - 0.5f) * expf(-6.0f * i / IR_LEN);
		e += ir[i] * ir[i];
	}
	for (int i = 0; i < IR_LEN; i++)
		ir[i] /= sqrtf(e) * 4.f;

	memset(near_sig, 0, sizeof near_sig);
	for (int i = DELAY; i < N; i++) {
		float echo = 0.f;
		for (int k = 0; k < IR_LEN; k++)
			echo += ir[k] * far_sig[i - DELAY - k];
		near_sig[i] = echo;
		if ((i / (RATE / 2)) % 2 == 1)		/* speech: 0.5s on / 0.5s off */
			near_sig[i] += 0.25f * sinf(2 * 3.14159f * 180.0f * i / (float) RATE)
				     + 0.05f * (rand() / (float) RAND_MAX - 0.5f);
		near_sig[i] += 0.004f * (rand() / (float) RAND_MAX - 0.5f);
	}
}

static double run_variant(int avx2, unsigned seed, float *out)
{
	const float *rec[2] = { near_sig, near_sig };
	const float *play[2] = { far_sig, far_sig };
	float *oo[2] = { out, out + N };

	aecn_state st;
	aecn_alloc(&st, RATE, 2, 80.f, 600.f, 0.35f);
	st.avx2 = avx2;
	double t0 = now();
	aecn_process(&st, rec, play, oo, N);
	double dt = now() - t0;

	/* ERLE over the last far-only window: speech is off when (i/24000)%2==0 */
	double echo_before = 0.f, echo_after = 0.f;
	for (int i = N - RATE; i < N; i++) {
		if ((i / (RATE / 2)) % 2 == 0) {
			echo_before += (double) near_sig[i] * near_sig[i];
			echo_after += (double) out[i] * out[i];
		}
	}
	double erle = 10.0 * log10((echo_before + 1e-20) / (echo_after + 1e-20));
	printf("  %-6s: %6.3fs CPU for %ds audio  ->  %5.2f%% of one core   ERLE %.1f dB\n",
	       avx2 ? "AVX2" : "scalar", dt, SECS, dt / SECS * 100.0, erle);
	printf("    delay tracked: %u samples (%.1f ms), corr %.3f\n",
	       st.delay, st.delay * 1000.0f / RATE, st.last_r);
	aecn_free(&st);
	return dt;
}

int main(void)
{
	static float out_a[2 * N], out_s[2 * N];
	build_signals(1234);
	printf("scenario: 48kHz stereo, far=music, near=echo@200ms+speech+noise, %ds\n", SECS);
	double ts = run_variant(0, 1234, out_s);
	double ta = run_variant(1, 1234, out_a);
	float md = 0.f;
	for (int i = 0; i < 2 * N; i++) {
		float d = fabsf(out_a[i] - out_s[i]);
		if (d > md) md = d;
	}
	printf("  speedup %.1fx; scalar-vs-AVX2 max sample diff %.6f\n", ts / ta, md);
	return 0;
}