/* pw-aec-avx: hosts the native module-echo-cancel in a tiny client process.
 *
 * All four audio paths of module-echo-cancel are pw_streams, so a plain
 * client process can host the module: the streams attach to the RUNNING
 * PipeWire graph, no daemon restart, instant on/off. Stopping = killing
 * this process (nodes vanish, zero CPU).
 *
 * usage: aec-host <backend-library> [aec-args]
 *   aec-host aec/libspa-aec-avx "tail_ms=80 delay_max_ms=600 mu=0.35"
 *   aec-host aec/libspa-aec-webrtc ""
 *
 * monitor.mode=true: AEC reference = monitor of the DEFAULT sink, so
 * playback may go to any output device and is still cancelled.
 */
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static struct pw_main_loop *loop;

static void on_sig(int sig)
{
	(void)sig;
	if (loop)
		pw_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
	const char *lib, *aecargs;
	struct pw_context *ctx;
	struct pw_core *core;
	struct pw_impl_module *mod;
	char args[1024];
	size_t n;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <backend-library> [aec-args]\n"
				"  e.g. %s aec/libspa-aec-avx \"tail_ms=80 delay_max_ms=600 mu=0.35\"\n",
				argv[0], argv[0]);
		return 1;
	}
	lib = argv[1];
	aecargs = argc > 2 ? argv[2] : "";

	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	if (loop == NULL) {
		fprintf(stderr, "aec-host: main loop failed\n");
		return 1;
	}
	ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	if (ctx == NULL)
		return 1;
	core = pw_context_connect(ctx, NULL, 0);
	if (core == NULL) {
		fprintf(stderr, "aec-host: cannot connect to PipeWire\n");
		return 1;
	}

	n = (size_t)snprintf(args, sizeof args,
			"library.name=%s "
			"monitor.mode=true "
			"source.props={ node.name = \"echo_cancel_source\" "
			"node.description = \"Mikrofon (echo-cancel)\" priority.session = 5000 }",
			lib);
	if (aecargs[0] != '\0')
		snprintf(args + n, sizeof args - n, " aec.args=\"%s\"", aecargs);

	mod = pw_context_load_module(ctx, "libpipewire-module-echo-cancel", args, NULL);
	if (mod == NULL) {
		fprintf(stderr, "aec-host: libpipewire-module-echo-cancel load failed (backend: %s)\n", lib);
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	pw_main_loop_run(loop);

	pw_context_destroy(ctx);
	pw_deinit();
	return 0;
}