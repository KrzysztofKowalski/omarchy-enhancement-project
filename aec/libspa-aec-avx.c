/* PipeWire SPA AEC backend: NLMS with delay tracking, AVX2/FMA accelerated.
 *
 * Load with module-echo-cancel:
 *     args = {
 *         library.name = aec/libspa-aec-avx
 *         # aec.args = "tail_ms=80 delay_max_ms=600 mu=0.35"
 *     }
 */
#include <spa/interfaces/audio/aec.h>
#include <spa/support/log.h>
#include <spa/support/plugin.h>
#include <spa/utils/string.h>
#include <spa/utils/names.h>

#include "aecn.h"

struct impl {
	struct spa_handle handle;
	struct spa_audio_aec aec;
	struct spa_log *log;

	aecn_state st;
	uint32_t channels;
};

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.aec.avx");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

static int avx_init(void *object, const struct spa_dict *args, const struct spa_audio_info_raw *info)
{
	struct impl *impl = object;
	const char *str;
	float tail_ms = 80.f, delay_max_ms = 600.f, mu = 0.35f;

	if (args) {
		if ((str = spa_dict_lookup(args, "tail_ms")) != NULL)
			tail_ms = (float) atof(str);
		if ((str = spa_dict_lookup(args, "delay_max_ms")) != NULL)
			delay_max_ms = (float) atof(str);
		if ((str = spa_dict_lookup(args, "mu")) != NULL)
			mu = (float) atof(str);
	}

	impl->channels = info->channels;
	if (info->channels == 0)
		return -EINVAL;
	if (aecn_alloc(&impl->st, info->rate, info->channels, tail_ms, delay_max_ms, mu) < 0)
		return -ENOMEM;

	spa_log_info(impl->log, "aec-avx: rate %u channels %u tail %u taps, delay range %u-%u, %s kernels",
			(unsigned) info->rate, (unsigned) info->channels,
			(unsigned) impl->st.tail, (unsigned) impl->st.min_delay,
			(unsigned) impl->st.max_delay, impl->st.avx2 ? "AVX2/FMA" : "scalar");
	return 0;
}

static int avx_run(void *object, const float *rec[], const float *play[], float *out[], uint32_t n_samples)
{
	struct impl *impl = object;
	if (play == NULL || play[0] == NULL) {
		for (uint32_t c = 0; c < impl->channels; c++)
			memcpy(out[c], rec[c], n_samples * sizeof(float));
		return 0;
	}
	aecn_process(&impl->st, rec, play, out, n_samples);
	return 0;
}

static const struct spa_audio_aec_methods impl_aec = {
	SPA_VERSION_AUDIO_AEC,
	.init = avx_init,
	.run = avx_run,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	struct impl *impl = (struct impl *) handle;
	if (spa_streq(type, SPA_TYPE_INTERFACE_AUDIO_AEC))
		*interface = &impl->aec;
	else
		return -ENOENT;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	aecn_free(&((struct impl *) handle)->st);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory, const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory, struct spa_handle *handle,
	  const struct spa_dict *info, const struct spa_support *support, uint32_t n_support)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	struct impl *impl = (struct impl *) handle;

	impl->aec.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_AUDIO_AEC,
		SPA_VERSION_AUDIO_AEC,
		&impl_aec, impl);
	impl->aec.name = "avx";
	impl->aec.info = NULL;
	impl->aec.latency = NULL;

	impl->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	spa_log_topic_init(impl->log, &log_topic);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_AUDIO_AEC,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*info = &impl_interfaces[*index];
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}

static const struct spa_handle_factory spa_aec_avx_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_AEC,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

SPA_LOG_TOPIC_ENUM_DEFINE_REGISTERED;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &spa_aec_avx_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}