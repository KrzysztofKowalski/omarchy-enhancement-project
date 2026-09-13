/* NLMS acoustic echo canceller with delay tracking.
 *
 * Monophonic core: far-end (speaker reference) and near-end (mic) are mixed
 * down; the residual is written back to every channel. Delay between the
 * far reference and the echo heard by the mic (e.g. Bluetooth codec latency)
 * is tracked by normalized cross-correlation and absorbed by a pre-delay
 * ring buffer, so the adaptive NLMS filter only has to cover the acoustic
 * tail (~tens of ms).
 *
 * Two kernels per hot loop: scalar baseline and AVX2+FMA, dispatched at
 * runtime (aecn_state.avx2), so the same .so runs everywhere.
 */
#ifndef AECN_H
#define AECN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define AECN_X86 1
#endif

#define AECN_CHUNK 1024
#define AECN_CORR_W 2048

typedef struct {
	/* config */
	uint32_t rate, channels;
	uint32_t tail;		/* adaptive FIR taps, multiple of 8 */
	uint32_t max_delay;	/* far->near delay search range, samples */
	uint32_t min_delay;
	float mu, eps, gamma;

	/* far/near history rings (mono) */
	uint32_t far_len, far_w;
	float *far;
	uint32_t near_len, near_w;
	float *near;

	/* linear scratch */
	float *fbuf;		/* tail + AECN_CHUNK - 1 samples */
	float *wrev;		/* reversed NLMS weights: wrev[m] = w[tail-1-m] */
	float *cbuf;		/* max_delay + AECN_CORR_W samples */
	float *nwin, *nwin2;	/* correlation window, whitened */
	float *fwin2;		/* whitened far window scratch */
	float *mn, *mf;		/* per-chunk mono near/far */

	/* delay tracker */
	uint32_t delay;
	float last_r;
	uint32_t cand, cand_hold;

	float efloor;
	uint64_t n;		/* absolute samples processed */
	int avx2;
} aecn_state;

/* ---- scalar kernels ---- */

static float aecn_dot(const float *a, const float *b, uint32_t n)
{
	float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
	uint32_t i = 0;
	for (; i + 4 <= n; i += 4) {
		s0 += a[i] * b[i];
		s1 += a[i + 1] * b[i + 1];
		s2 += a[i + 2] * b[i + 2];
		s3 += a[i + 3] * b[i + 3];
	}
	for (; i < n; i++)
		s0 += a[i] * b[i];
	return (s0 + s1) + (s2 + s3);
}

static void aecn_axpy(float *a, const float *b, float alpha, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		a[i] += alpha * b[i];
}

static float aecn_maxabs(const float *a, uint32_t n)
{
	float m = 0.f;
	for (uint32_t i = 0; i < n; i++) {
		float v = fabsf(a[i]);
		if (v > m)
			m = v;
	}
	return m;
}

#ifdef AECN_X86
/* ---- AVX2+FMA versions, only entered when dispatch selects them ---- */

__attribute__((target("avx2,fma")))
static float aecn_dot_avx2(const float *a, const float *b, uint32_t n)
{
	__m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
	__m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
	uint32_t i = 0;
	for (; i + 32 <= n; i += 32) {
		a0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), a0);
		a1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), a1);
		a2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), a2);
		a3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), a3);
	}
	for (; i + 8 <= n; i += 8)
		a0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), a0);
	__m256 t = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
	__m128 h = _mm_add_ps(_mm256_castps256_ps128(t), _mm256_extractf128_ps(t, 1));
	h = _mm_add_ps(h, _mm_movehl_ps(h, h));
	float out = _mm_cvtss_f32(_mm_add_ss(h, _mm_shuffle_ps(h, h, _MM_SHUFFLE(1, 0, 1, 0))));
	for (; i < n; i++)
		out += a[i] * b[i];
	return out;
}

__attribute__((target("avx2,fma"), noinline))
static void aecn_axpy_avx2(float *a, const float *b, float alpha, uint32_t n)
{
	const __m256 av = _mm256_set1_ps(alpha);
	uint32_t i = 0;
	for (; i + 32 <= n; i += 32) {
		_mm256_storeu_ps(a + i,
			_mm256_fmadd_ps(av, _mm256_loadu_ps(b + i), _mm256_loadu_ps(a + i)));
		_mm256_storeu_ps(a + i + 8,
			_mm256_fmadd_ps(av, _mm256_loadu_ps(b + i + 8), _mm256_loadu_ps(a + i + 8)));
		_mm256_storeu_ps(a + i + 16,
			_mm256_fmadd_ps(av, _mm256_loadu_ps(b + i + 16), _mm256_loadu_ps(a + i + 16)));
		_mm256_storeu_ps(a + i + 24,
			_mm256_fmadd_ps(av, _mm256_loadu_ps(b + i + 24), _mm256_loadu_ps(a + i + 24)));
	}
	for (; i + 8 <= n; i += 8)
		_mm256_storeu_ps(a + i,
			_mm256_fmadd_ps(av, _mm256_loadu_ps(b + i), _mm256_loadu_ps(a + i)));
	for (; i < n; i++)
		a[i] += alpha * b[i];
}

__attribute__((target("avx2,fma")))
static float aecn_maxabs_avx2(const float *a, uint32_t n)
{
	const __m256 sign = _mm256_set1_ps(-0.f);
	__m256 m = _mm256_setzero_ps();
	uint32_t i = 0;
	for (; i + 8 <= n; i += 8) {
		__m256 v = _mm256_andnot_ps(sign, _mm256_loadu_ps(a + i));
		m = _mm256_max_ps(m, v);
	}
	__m128 h = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
	h = _mm_max_ps(h, _mm_movehl_ps(h, h));
	float out = _mm_cvtss_f32(_mm_max_ss(h, _mm_shuffle_ps(h, h, _MM_SHUFFLE(1, 0, 1, 0))));
	for (; i < n; i++) {
		float v = fabsf(a[i]);
		if (v > out)
			out = v;
	}
	return out;
}
#endif

/* ---- dispatchers ---- */

static inline float aecn_dot_d(const aecn_state *s, const float *a, const float *b, uint32_t n)
{
#ifdef AECN_X86
	if (s->avx2)
		return aecn_dot_avx2(a, b, n);
#endif
	return aecn_dot(a, b, n);
}

static inline void aecn_axpy_d(const aecn_state *s, float *a, const float *b, float alpha, uint32_t n)
{
#ifdef AECN_X86
	if (s->avx2) {
		aecn_axpy_avx2(a, b, alpha, n);
		return;
	}
#endif
	aecn_axpy(a, b, alpha, n);
}

static inline float aecn_maxabs_d(const aecn_state *s, const float *a, uint32_t n)
{
#ifdef AECN_X86
	if (s->avx2)
		return aecn_maxabs_avx2(a, n);
#endif
	return aecn_maxabs(a, n);
}

/* ---- rings ---- */

static inline void aecn_ring_append(float *ring, uint32_t len, uint32_t *wpos,
				    const float *src, uint32_t cnt)
{
	uint32_t w = *wpos % len;
	uint32_t first = len - w;
	if (first > cnt)
		first = cnt;
	memcpy(ring + w, src, first * sizeof(float));
	memcpy(ring, src + first, (cnt - first) * sizeof(float));
	*wpos = (*wpos + cnt) % len;
}

/* Read cnt samples starting at absolute index `start` (may wrap in ring space). */
static inline void aecn_ring_read(const float *ring, uint32_t len, uint64_t start,
				  float *dst, uint32_t cnt)
{
	uint32_t p0 = (uint32_t) (start % len);
	uint32_t first = len - p0;
	if (first > cnt)
		first = cnt;
	memcpy(dst, ring + p0, first * sizeof(float));
	memcpy(dst + first, ring, (cnt - first) * sizeof(float));
}

/* ---- delay tracker ---- */

/* Pre-whiten with a first difference: kills the tonal/periodic structure of
 * music and the dominance of low-frequency speech, so the normalized
 * cross-correlation picks the true echo lag instead of a harmonic alias. */
static void aecn_whiten(const float *x, float *dst, uint32_t n)
{
	dst[0] = 0.f;
	for (uint32_t i = 1; i < n; i++)
		dst[i] = x[i] - x[i - 1];
}

static void aecn_estimate(aecn_state *s)
{
	const uint32_t W = AECN_CORR_W;
	if (s->n < (uint64_t) s->max_delay + W + 16)
		return;
	aecn_ring_read(s->near, s->near_len, s->n - W, s->nwin, W);
	aecn_whiten(s->nwin, s->nwin2, W);
	float En = aecn_dot_d(s, s->nwin2, s->nwin2, W);
	if (En < 1e-9f * W)
		return;
	aecn_ring_read(s->far, s->far_len, s->n - (uint64_t) s->max_delay - W + 1,
		       s->cbuf, s->max_delay + W);

	/* far gate: with no playback there is nothing to correlate against -
	 * skip the search (idle-mic chatter would otherwise keep it spinning) */
	if (aecn_dot_d(s, s->cbuf + s->max_delay, s->cbuf + s->max_delay, W)
	    < 1e-8f * W)
		return;

	float best = 0.f;
	uint32_t bl = s->delay;
	/* cbuf[k] = far(n - max_delay - W + 1 + k); lag L -> far window at cbuf + (max_delay - L).
	 * Both sides whitened (first difference), correlation + energy in one pass. */
	for (uint32_t L = s->min_delay; L + 96 <= s->max_delay; L += 96) {
		const float *fp = s->cbuf + (s->max_delay - L);
		float c = 0.f, Ef = 0.f;
		for (uint32_t i = 1; i < W; i++) {
			float d = fp[i] - fp[i - 1];
			c += s->nwin2[i] * d;
			Ef += d * d;
		}
		float r = fabsf(c) / sqrtf(En * Ef + 1e-12f);
		if (abs((int) L - (int) s->delay) > 4800)
			r -= 0.01f;
		if (r > best) {
			best = r;
			bl = L;
		}
	}
	/* refine +-48 samples around the coarse peak */
	uint32_t lo = bl > 48 ? bl - 48 : s->min_delay;
	uint32_t hi = bl + 48;
	if (hi > s->max_delay)
		hi = s->max_delay;
	for (uint32_t L = lo; L <= hi; L += 8) {
		const float *fp = s->cbuf + (s->max_delay - L);
		float c = 0.f, Ef = 0.f;
		for (uint32_t i = 1; i < W; i++) {
			float d = fp[i] - fp[i - 1];
			c += s->nwin2[i] * d;
			Ef += d * d;
		}
		float r = fabsf(c) / sqrtf(En * Ef + 1e-12f);
		if (abs((int) L - (int) s->delay) > 4800)
			r -= 0.01f;
		if (r > best) {
			best = r;
			bl = L;
		}
	}

	/* stability gate: a candidate must win twice in a row before committing */
	if (best < 0.40f) {
		s->cand_hold = 0;
		return;
	}
	if (abs((int) bl - (int) s->cand) <= 16)
		s->cand_hold++;
	else {
		s->cand = bl;
		s->cand_hold = 1;
	}
	if (s->cand_hold >= 2 && (best >= s->last_r || s->last_r < 0.3f)) {
		if (bl != s->delay) {
			/* big jump: the old weights point at a stale echo path */
			if (bl > s->delay + 128 || bl + 128 < s->delay)
				memset(s->wrev, 0, s->tail * sizeof(float));
			s->delay = bl;
		}
		s->last_r = best;
	}
}

/* ---- lifecycle ---- */

static int aecn_alloc(aecn_state *s, uint32_t rate, uint32_t channels,
		      float tail_ms, float delay_max_ms, float mu)
{
	memset(s, 0, sizeof *s);
	s->rate = rate;
	s->channels = channels;
	s->tail = (uint32_t) (tail_ms * rate / 1000.0f) & ~7u;
	if (s->tail < 64)
		s->tail = 64;
	s->min_delay = 2048;
	s->max_delay = (uint32_t) (delay_max_ms * rate / 1000.0f);
	if (s->max_delay < s->min_delay + AECN_CORR_W)
		s->max_delay = s->min_delay + AECN_CORR_W;
	s->mu = mu;
	s->eps = 1e-6f;
	s->gamma = 0.6f;
	s->delay = s->min_delay;
	s->efloor = 1e-8f * s->tail;
	s->far_len = s->max_delay + AECN_CHUNK + s->tail + 64;
	s->near_len = 4096;

	s->far = calloc(s->far_len, sizeof(float));
	s->near = calloc(s->near_len, sizeof(float));
	s->fbuf = malloc((s->tail + AECN_CHUNK + 8) * sizeof(float));
	s->wrev = calloc(s->tail, sizeof(float));
	s->cbuf = malloc(((size_t) s->max_delay + AECN_CORR_W) * sizeof(float));
	s->nwin = malloc(AECN_CORR_W * sizeof(float));
	s->nwin2 = malloc(AECN_CORR_W * sizeof(float));
	s->mn = malloc(AECN_CHUNK * sizeof(float));
	s->mf = malloc(AECN_CHUNK * sizeof(float));
	if (!s->far || !s->near || !s->fbuf || !s->wrev || !s->cbuf ||
	    !s->nwin || !s->nwin2 || !s->mn || !s->mf)
		return -1;

#ifdef AECN_X86
	__builtin_cpu_init();
	s->avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
	return 0;
}

static void aecn_free(aecn_state *s)
{
	free(s->far);
	free(s->near);
	free(s->fbuf);
	free(s->wrev);
	free(s->cbuf);
	free(s->nwin);
	free(s->nwin2);
	free(s->mn);
	free(s->mf);
	memset(s, 0, sizeof *s);
}

/* ---- processing ---- */

static void aecn_process(aecn_state *s,
			 const float *rec[], const float *play[],
			 float *out[], uint32_t n_samples)
{
	const uint32_t ch = s->channels, tail = s->tail;
	const float inv_ch = 1.0f / ch;
	uint32_t done = 0;

	while (done < n_samples) {
		uint32_t blk = n_samples - done;
		if (blk > AECN_CHUNK)
			blk = AECN_CHUNK;

		/* mono mixdown of this chunk */
		for (uint32_t i = 0; i < blk; i++) {
			float fn = 0.f, ff = 0.f;
			for (uint32_t c = 0; c < ch; c++) {
				fn += rec[c][done + i];
				ff += play ? play[c][done + i] : 0.f;
			}
			s->mn[i] = fn * inv_ch;
			s->mf[i] = ff * inv_ch;
		}
		aecn_ring_append(s->far, s->far_len, &s->far_w, s->mf, blk);
		aecn_ring_append(s->near, s->near_len, &s->near_w, s->mn, blk);

		if (s->n + blk < (uint64_t) s->tail + s->delay + 1) {
			/* not enough far history yet: pass the mic through */
			for (uint32_t i = 0; i < blk; i++)
				for (uint32_t c = 0; c < ch; c++)
					out[c][done + i] = rec[c][done + i];
		} else {
			/* fbuf[i] = far(n0 - delay - tail + 1 + i), i < tail + blk - 1 */
			uint64_t a = s->n - (uint64_t) s->delay - tail + 1;
			aecn_ring_read(s->far, s->far_len, a, s->fbuf, tail + blk - 1);
			float maxf = aecn_maxabs_d(s, s->fbuf, tail + blk - 1);

			/* digital silence on the far side: y == 0, nothing to adapt -
			 * pass the (mono) mic through without paying for the FIR */
			if (maxf < 1e-6f) {
				for (uint32_t i = 0; i < blk; i++)
					for (uint32_t c = 0; c < ch; c++)
						out[c][done + i] = s->mn[i];
				s->n += blk;
				done += blk;
				continue;
			}

			float E = aecn_dot_d(s, s->fbuf, s->fbuf, tail);

			for (uint32_t j = 0; j < blk; j++) {
				if ((s->n + j) % 4096 == 0)
					E = aecn_dot_d(s, s->fbuf + j, s->fbuf + j, tail);
				float y = aecn_dot_d(s, s->wrev, s->fbuf + j, tail);
				float e = s->mn[j] - y;
				if (E >= s->efloor && fabsf(s->mn[j]) < s->gamma * maxf) {
					float alpha = s->mu * e / (E + s->eps);
					aecn_axpy_d(s, s->wrev, s->fbuf + j, alpha, tail);
				}
				for (uint32_t c = 0; c < ch; c++)
					out[c][done + j] = e;
				/* sliding window energy */
				E += s->fbuf[j + tail] * s->fbuf[j + tail] - s->fbuf[j] * s->fbuf[j];
			}
		}

		s->n += blk;
		done += blk;

		/* delay re-estimation every ~50ms (faster during acquisition) */
		uint32_t iv = s->n < s->rate ? 600 : 24000;
		if (s->n % iv < blk)
			aecn_estimate(s);
	}
}

#endif /* AECN_H */