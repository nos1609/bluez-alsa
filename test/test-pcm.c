/*
 * test-pcm.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 * This program might be used to debug or check the functionality of ALSA
 * plug-ins. It should work exactly the same as the BlueALSA server. When
 * connecting to the bluealsa device, one should use "hci-test" interface.
 *
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "test.inc"
#include "utils.inc"

#include "../src/bluealsa.c"
#include "../src/ctl.c"
#include "../src/io.h"
#define io_thread_a2dp_sink_sbc _io_thread_a2dp_sink_sbc
#define io_thread_a2dp_source_sbc _io_thread_a2dp_source_sbc
#include "../src/io.c"
#undef io_thread_a2dp_sink_sbc
#undef io_thread_a2dp_source_sbc
#define transport_acquire_bt_a2dp _transport_acquire_bt_a2dp
#include "../src/transport.c"
#undef transport_acquire_bt_a2dp
#include "../src/utils.c"

static const a2dp_sbc_t cconfig = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = MIN_BITPOOL,
	.max_bitpool = MAX_BITPOOL,
};

static char *drum_buffer;
static size_t drum_buffer_size;

static void test_pcm_setup_free(void) {
	bluealsa_ctl_free();
	bluealsa_config_free();
}

static void test_pcm_setup_free_handler(int sig) {
	(void)(sig);
	test_pcm_setup_free();
}

int transport_acquire_bt_a2dp(struct ba_transport *t) {
	t->state = TRANSPORT_ACTIVE;
	assert(io_thread_create(t) == 0);
	return 0;
}

void *io_thread_a2dp_sink_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	int16_t *head = (int16_t *)drum_buffer;
	int16_t *end = head + drum_buffer_size / sizeof(int16_t);

	struct sigaction sigact = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &sigact, NULL);

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	while (TRANSPORT_RUN_IO_THREAD(t)) {

		if (io_thread_open_pcm_write(&t->a2dp.pcm) == -1) {
			if (errno != ENXIO)
				error("Couldn't open FIFO: %s", strerror(errno));
			usleep(10000);
			continue;
		}

		fprintf(stderr, ".");

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		if (head == end)
			head = (int16_t *)drum_buffer;

		size_t samples = head + 512 > end ? end - head : 512;
		if (io_thread_write_pcm(&t->a2dp.pcm, head, samples) == -1)
			error("FIFO write error: %s", strerror(errno));

		head += samples;
		io_thread_time_sync(&io_sync, samples / 2);
	}

	return NULL;
}

void *io_thread_a2dp_source_sbc(void *arg) {
	struct ba_transport *t = (struct ba_transport *)arg;

	while ((t->a2dp.pcm.fd = open(t->a2dp.pcm.fifo, O_RDONLY)) == -1)
		usleep(10000);

	int16_t buffer[1024 * 2];
	ssize_t samples;

	struct io_sync io_sync = {
		.sampling = transport_get_sampling(t),
	};

	while (TRANSPORT_RUN_IO_THREAD(t)) {
		fprintf(stderr, ".");

		if (io_sync.frames == 0)
			clock_gettime(CLOCK_MONOTONIC, &io_sync.ts0);

		const size_t in_samples = sizeof(buffer) / sizeof(int16_t);
		if ((samples = io_thread_read_pcm(&t->a2dp.pcm, buffer, in_samples)) <= 0) {
			if (samples == -1)
				error("FIFO read error: %s", strerror(errno));
			break;
		}

		io_thread_time_sync(&io_sync, samples / 2);
	}

	return NULL;
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "hsit:";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "source", no_argument, NULL, 's' },
		{ "sink", no_argument, NULL, 'i' },
		{ "timeout", required_argument, NULL, 't' },
		{ 0, 0, 0, 0 },
	};

	int source = 0;
	int sink = 0;
	int timeout = 5;

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h':
			printf("usage: %s [--source] [--sink] [--timeout SEC]\n", argv[0]);
			return EXIT_SUCCESS;
		case 's':
			source = 1;
			break;
		case 'i':
			sink = 1;
			break;
		case 't':
			timeout = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	/* emulate dummy test HCI device */
	strcpy(config.hci_dev.name, "hci-test");

	assert(bluealsa_config_init() == 0);
	if ((bluealsa_ctl_thread_init() == -1)) {
		perror("ctl_thread_init");
		return EXIT_FAILURE;
	}

	/* make sure to cleanup named pipes */
	struct sigaction sigact = { .sa_handler = test_pcm_setup_free_handler };
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	atexit(test_pcm_setup_free);

	bdaddr_t addr;
	struct ba_device *d;

	str2ba("12:34:56:78:9A:BC", &addr);
	assert((d = device_new(1, &addr, "Test Device")) != NULL);
	g_hash_table_insert(config.devices, g_strdup("/device"), d);

	if (source) {
		struct ba_transport *t_source;
		assert((t_source = transport_new_a2dp(d, ":test", "/source",
						BLUETOOTH_PROFILE_A2DP_SOURCE, A2DP_CODEC_SBC,
						(uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
	}

	if (sink) {
		struct ba_transport *t_sink;
		assert((t_sink = transport_new_a2dp(d, ":test", "/sink",
						BLUETOOTH_PROFILE_A2DP_SINK, A2DP_CODEC_SBC,
						(uint8_t *)&cconfig, sizeof(cconfig))) != NULL);
		assert(load_file(SRCDIR "/drum.raw", &drum_buffer, &drum_buffer_size) == 0);
	}

	sleep(timeout);
	return EXIT_SUCCESS;
}
