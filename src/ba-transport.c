/*
 * BlueALSA - ba-transport.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "ba-transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include <gio/gunixfdlist.h>

#include "a2dp-codecs.h"
#include "bluealsa.h"
#include "bluez-iface.h"
#include "ctl.h"
#include "hfp.h"
#include "io.h"
#include "rfcomm.h"
#include "utils.h"
#include "shared/log.h"

static int io_thread_create(struct ba_transport *t) {

	void *(*routine)(void *) = NULL;
	int ret;

	if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM)
		routine = rfcomm_thread;
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		routine = io_thread_sco;
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			routine = io_thread_a2dp_source_sbc;
			break;
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			routine = io_thread_a2dp_source_aac;
			break;
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			routine = io_thread_a2dp_source_aptx;
			break;
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			routine = io_thread_a2dp_source_ldac;
			break;
#endif
		default:
			warn("Codec not supported: %u", t->type.codec);
		}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			routine = io_thread_a2dp_sink_sbc;
			break;
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			routine = io_thread_a2dp_sink_aac;
			break;
#endif
		default:
			warn("Codec not supported: %u", t->type.codec);
		}

	if (routine == NULL)
		return -1;

	if ((ret = pthread_create(&t->thread, NULL, routine, t)) != 0) {
		error("Couldn't create IO thread: %s", strerror(ret));
		t->thread = config.main_thread;
		return -1;
	}

	pthread_setname_np(t->thread, "baio");
	debug("Created new IO thread: %s", ba_transport_type_to_string(t->type));

	return 0;
}

/**
 * Create new transport.
 *
 * @param device Pointer to the device structure.
 * @param type Transport type.
 * @param dbus_owner D-Bus service, which owns this transport.
 * @param dbus_path D-Bus service path for this transport.
 * @param profile Bluetooth profile.
 * @return On success, the pointer to the newly allocated transport structure
 *   is returned. If error occurs, NULL is returned and the errno variable is
 *   set to indicated the cause of the error. */
struct ba_transport *transport_new(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;
	int err;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		goto fail;

	t->d = device;
	t->type = type;

	pthread_mutex_init(&t->mutex, NULL);

	t->state = TRANSPORT_IDLE;
	t->thread = config.main_thread;

	t->bt_fd = -1;
	t->sig_fd[0] = -1;
	t->sig_fd[1] = -1;

	if ((t->dbus_owner = strdup(dbus_owner)) == NULL)
		goto fail;
	if ((t->dbus_path = strdup(dbus_path)) == NULL)
		goto fail;

	if (pipe(t->sig_fd) == -1)
		goto fail;

	g_hash_table_insert(device->transports, t->dbus_path, t);
	return t;

fail:
	err = errno;
	ba_transport_free(t);
	errno = err;
	return NULL;
}

/* These acquire/release helper functions should be defined before the
 * corresponding transport_new_* ones. However, git commit history is
 * more important, so we're going to keep these functions at original
 * locations and use forward declarations instead. */
static int transport_acquire_bt_a2dp(struct ba_transport *t);
static int transport_release_bt_a2dp(struct ba_transport *t);
static int transport_release_bt_rfcomm(struct ba_transport *t);
static int transport_acquire_bt_sco(struct ba_transport *t);
static int transport_release_bt_sco(struct ba_transport *t);

struct ba_transport *transport_new_a2dp(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path,
		const uint8_t *cconfig,
		size_t cconfig_size) {

	struct ba_transport *t;

	if ((t = transport_new(device, type, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->a2dp.ch1_volume = 127;
	t->a2dp.ch2_volume = 127;

	if (cconfig_size > 0) {
		t->a2dp.cconfig = malloc(cconfig_size);
		t->a2dp.cconfig_size = cconfig_size;
		memcpy(t->a2dp.cconfig, cconfig, cconfig_size);
	}

	t->a2dp.pcm.fd = -1;
	t->a2dp.pcm.client = -1;
	pthread_mutex_init(&t->a2dp.drained_mtx, NULL);
	pthread_cond_init(&t->a2dp.drained, NULL);

	t->acquire = transport_acquire_bt_a2dp;
	t->release = transport_release_bt_a2dp;

	bluealsa_ctl_send_event(device->a->ctl, BA_EVENT_TRANSPORT_ADDED, &device->addr,
			BA_PCM_TYPE_A2DP | (type.profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE ?
				BA_PCM_STREAM_PLAYBACK : BA_PCM_STREAM_CAPTURE));

	return t;
}

struct ba_transport *transport_new_rfcomm(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t, *t_sco;
	char dbus_path_sco[64];

	struct ba_transport_type ttype = {
		.profile = type.profile | BA_TRANSPORT_PROFILE_RFCOMM };
	if ((t = transport_new(device, ttype, dbus_owner, dbus_path)) == NULL)
		goto fail;

	snprintf(dbus_path_sco, sizeof(dbus_path_sco), "%s/sco", dbus_path);
	if ((t_sco = transport_new_sco(device, type, dbus_owner, dbus_path_sco)) == NULL)
		goto fail;

	t->rfcomm.sco = t_sco;
	t_sco->sco.rfcomm = t;

	t->release = transport_release_bt_rfcomm;

	return t;

fail:
	ba_transport_free(t);
	return NULL;
}

struct ba_transport *transport_new_sco(
		struct ba_device *device,
		struct ba_transport_type type,
		const char *dbus_owner,
		const char *dbus_path) {

	struct ba_transport *t;

	/* HSP supports CVSD only */
	if (type.profile & BA_TRANSPORT_PROFILE_MASK_HSP)
		type.codec = HFP_CODEC_CVSD;

	if ((t = transport_new(device, type, dbus_owner, dbus_path)) == NULL)
		return NULL;

	t->sco.spk_gain = 15;
	t->sco.mic_gain = 15;

	t->sco.spk_pcm.fd = -1;
	t->sco.spk_pcm.client = -1;

	t->sco.mic_pcm.fd = -1;
	t->sco.mic_pcm.client = -1;

	pthread_mutex_init(&t->sco.spk_drained_mtx, NULL);
	pthread_cond_init(&t->sco.spk_drained, NULL);

	t->acquire = transport_acquire_bt_sco;
	t->release = transport_release_bt_sco;

	bluealsa_ctl_send_event(device->a->ctl, BA_EVENT_TRANSPORT_ADDED, &device->addr,
			BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE);

	return t;
}

struct ba_transport *ba_transport_lookup(
		struct ba_device *device,
		const char *dbus_path) {
#if DEBUG
	/* make sure that the device mutex is acquired */
	g_assert(pthread_mutex_trylock(&device->a->devices_mutex) == EBUSY);
#endif
	return g_hash_table_lookup(device->transports, dbus_path);
}

void ba_transport_free(struct ba_transport *t) {

	if (t == NULL || t->state == TRANSPORT_LIMBO)
		return;

	t->state = TRANSPORT_LIMBO;
	debug("Freeing transport: %s", ba_transport_type_to_string(t->type));

	/* If the transport is active, prior to releasing resources, we have to
	 * terminate the IO thread (or at least make sure it is not running any
	 * more). Not doing so might result in an undefined behavior or even a
	 * race condition (closed and reused file descriptor). */
	transport_pthread_cancel(t->thread);

	/* if possible, try to release resources gracefully */
	if (t->release != NULL)
		t->release(t);

	if (t->bt_fd != -1)
		close(t->bt_fd);
	if (t->sig_fd[0] != -1)
		close(t->sig_fd[0]);
	if (t->sig_fd[1] != -1)
		close(t->sig_fd[1]);

	pthread_mutex_destroy(&t->mutex);

	unsigned int pcm_type = BA_PCM_TYPE_NULL;
	struct ba_device *d = t->d;

	/* free profile-specific resources */
	if (t->type.profile & BA_TRANSPORT_PROFILE_RFCOMM) {
		memset(&d->battery, 0, sizeof(d->battery));
		memset(&d->xapl, 0, sizeof(d->xapl));
		ba_transport_free(t->rfcomm.sco);
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {
		pcm_type = BA_PCM_TYPE_SCO | BA_PCM_STREAM_PLAYBACK | BA_PCM_STREAM_CAPTURE;
		pthread_mutex_destroy(&t->sco.spk_drained_mtx);
		pthread_cond_destroy(&t->sco.spk_drained);
		transport_release_pcm(&t->sco.spk_pcm);
		transport_release_pcm(&t->sco.mic_pcm);
		if (t->sco.rfcomm != NULL)
			t->sco.rfcomm->rfcomm.sco = NULL;
	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {
		pcm_type = BA_PCM_TYPE_A2DP | (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE ?
				BA_PCM_STREAM_PLAYBACK : BA_PCM_STREAM_CAPTURE);
		transport_release_pcm(&t->a2dp.pcm);
		pthread_mutex_destroy(&t->a2dp.drained_mtx);
		pthread_cond_destroy(&t->a2dp.drained);
		free(t->a2dp.cconfig);
	}

	/* detach transport from the device */
	g_hash_table_steal(d->transports, t->dbus_path);

	if (pcm_type != BA_PCM_TYPE_NULL)
		bluealsa_ctl_send_event(d->a->ctl, BA_EVENT_TRANSPORT_REMOVED, &d->addr, pcm_type);

	free(t->dbus_owner);
	free(t->dbus_path);
	free(t);
}

int transport_send_signal(struct ba_transport *t, enum ba_transport_signal sig) {
	return write(t->sig_fd[1], &sig, sizeof(sig));
}

int transport_send_rfcomm(struct ba_transport *t, const char command[32]) {

	char msg[sizeof(enum ba_transport_signal) + 32];

	((enum ba_transport_signal *)msg)[0] = TRANSPORT_SEND_RFCOMM;
	memcpy(&msg[sizeof(enum ba_transport_signal)], command, 32);

	return write(t->sig_fd[1], msg, sizeof(msg));
}

unsigned int transport_get_channels(const struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->channel_mode) {
			case SBC_CHANNEL_MODE_MONO:
				return 1;
			case SBC_CHANNEL_MODE_STEREO:
			case SBC_CHANNEL_MODE_JOINT_STEREO:
			case SBC_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->channel_mode) {
			case MPEG_CHANNEL_MODE_MONO:
				return 1;
			case MPEG_CHANNEL_MODE_STEREO:
			case MPEG_CHANNEL_MODE_JOINT_STEREO:
			case MPEG_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (((a2dp_aac_t *)t->a2dp.cconfig)->channels) {
			case AAC_CHANNELS_1:
				return 1;
			case AAC_CHANNELS_2:
				return 2;
			}
			break;
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->channel_mode) {
			case APTX_CHANNEL_MODE_MONO:
				return 1;
			case APTX_CHANNEL_MODE_STEREO:
				return 2;
			}
			break;
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			switch (((a2dp_ldac_t *)t->a2dp.cconfig)->channel_mode) {
			case LDAC_CHANNEL_MODE_MONO:
				return 1;
			case LDAC_CHANNEL_MODE_STEREO:
			case LDAC_CHANNEL_MODE_DUAL_CHANNEL:
				return 2;
			}
			break;
#endif
		}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		return 1;

	/* the number of channels is unspecified */
	return 0;
}

unsigned int transport_get_sampling(const struct ba_transport *t) {

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		switch (t->type.codec) {
		case A2DP_CODEC_SBC:
			switch (((a2dp_sbc_t *)t->a2dp.cconfig)->frequency) {
			case SBC_SAMPLING_FREQ_16000:
				return 16000;
			case SBC_SAMPLING_FREQ_32000:
				return 32000;
			case SBC_SAMPLING_FREQ_44100:
				return 44100;
			case SBC_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
#if ENABLE_MPEG
		case A2DP_CODEC_MPEG12:
			switch (((a2dp_mpeg_t *)t->a2dp.cconfig)->frequency) {
			case MPEG_SAMPLING_FREQ_16000:
				return 16000;
			case MPEG_SAMPLING_FREQ_22050:
				return 22050;
			case MPEG_SAMPLING_FREQ_24000:
				return 24000;
			case MPEG_SAMPLING_FREQ_32000:
				return 32000;
			case MPEG_SAMPLING_FREQ_44100:
				return 44100;
			case MPEG_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
#endif
#if ENABLE_AAC
		case A2DP_CODEC_MPEG24:
			switch (AAC_GET_FREQUENCY(*(a2dp_aac_t *)t->a2dp.cconfig)) {
			case AAC_SAMPLING_FREQ_8000:
				return 8000;
			case AAC_SAMPLING_FREQ_11025:
				return 11025;
			case AAC_SAMPLING_FREQ_12000:
				return 12000;
			case AAC_SAMPLING_FREQ_16000:
				return 16000;
			case AAC_SAMPLING_FREQ_22050:
				return 22050;
			case AAC_SAMPLING_FREQ_24000:
				return 24000;
			case AAC_SAMPLING_FREQ_32000:
				return 32000;
			case AAC_SAMPLING_FREQ_44100:
				return 44100;
			case AAC_SAMPLING_FREQ_48000:
				return 48000;
			case AAC_SAMPLING_FREQ_64000:
				return 64000;
			case AAC_SAMPLING_FREQ_88200:
				return 88200;
			case AAC_SAMPLING_FREQ_96000:
				return 96000;
			}
			break;
#endif
#if ENABLE_APTX
		case A2DP_CODEC_VENDOR_APTX:
			switch (((a2dp_aptx_t *)t->a2dp.cconfig)->frequency) {
			case APTX_SAMPLING_FREQ_16000:
				return 16000;
			case APTX_SAMPLING_FREQ_32000:
				return 32000;
			case APTX_SAMPLING_FREQ_44100:
				return 44100;
			case APTX_SAMPLING_FREQ_48000:
				return 48000;
			}
			break;
#endif
#if ENABLE_LDAC
		case A2DP_CODEC_VENDOR_LDAC:
			switch (((a2dp_ldac_t *)t->a2dp.cconfig)->frequency) {
			case LDAC_SAMPLING_FREQ_44100:
				return 44100;
			case LDAC_SAMPLING_FREQ_48000:
				return 48000;
			case LDAC_SAMPLING_FREQ_88200:
				return 88200;
			case LDAC_SAMPLING_FREQ_96000:
				return 96000;
			case LDAC_SAMPLING_FREQ_176400:
				return 176400;
			case LDAC_SAMPLING_FREQ_192000:
				return 192000;
			}
			break;
#endif
		}

	if (IS_BA_TRANSPORT_PROFILE_SCO(t->type.profile))
		switch (t->type.codec) {
		case HFP_CODEC_UNDEFINED:
			break;
		case HFP_CODEC_CVSD:
			return 8000;
		case HFP_CODEC_MSBC:
			return 16000;
		default:
			debug("Unsupported SCO codec: %#x", t->type.codec);
		}

	/* the sampling frequency is unspecified */
	return 0;
}

int transport_set_state(struct ba_transport *t, enum ba_transport_state state) {
	debug("State transition: %d -> %d", t->state, state);

	if (t->state == state)
		return 0;

	/* For the A2DP sink profile, the IO thread can not be created until the
	 * BT transport is acquired, otherwise thread initialized will fail. */
	if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK &&
			t->state == TRANSPORT_IDLE && state != TRANSPORT_PENDING)
		return 0;

	int ret = 0;

	t->state = state;

	switch (state) {
	case TRANSPORT_IDLE:
		transport_pthread_cancel(t->thread);
		break;
	case TRANSPORT_PENDING:
		/* When transport is marked as pending, try to acquire transport, but only
		 * if we are handing A2DP sink profile. For source profile, transport has
		 * to be acquired by our controller (during the PCM open request). */
		if (t->type.profile == BA_TRANSPORT_PROFILE_A2DP_SINK)
			ret = t->acquire(t);
		break;
	case TRANSPORT_ACTIVE:
	case TRANSPORT_PAUSED:
		if (pthread_equal(t->thread, config.main_thread))
			ret = io_thread_create(t);
		break;
	case TRANSPORT_LIMBO:
		break;
	}

	/* something went wrong, so go back to idle */
	if (ret == -1)
		return transport_set_state(t, TRANSPORT_IDLE);

	return ret;
}

int transport_drain_pcm(struct ba_transport *t) {

	pthread_mutex_t *mutex = NULL;
	pthread_cond_t *drained = NULL;

	switch (t->type.profile) {
	case BA_TRANSPORT_PROFILE_A2DP_SOURCE:
		mutex = &t->a2dp.drained_mtx;
		drained = &t->a2dp.drained;
		break;
	case BA_TRANSPORT_PROFILE_HFP_AG:
	case BA_TRANSPORT_PROFILE_HSP_AG:
		mutex = &t->sco.spk_drained_mtx;
		drained = &t->sco.spk_drained;
		break;
	}

	if (mutex == NULL || t->state != TRANSPORT_ACTIVE)
		return 0;

	pthread_mutex_lock(mutex);

	transport_send_signal(t, TRANSPORT_PCM_SYNC);
	pthread_cond_wait(drained, mutex);

	pthread_mutex_unlock(mutex);

	/* TODO: Asynchronous transport release.
	 *
	 * Unfortunately, BlueZ does not provide API for internal buffer drain.
	 * Also, there is no specification for Bluetooth playback drain. In order
	 * to make sure, that all samples are played out, we have to wait some
	 * arbitrary time before releasing transport. In order to make it right,
	 * there is a requirement for an asynchronous release mechanism, which
	 * is not implemented - it requires a little bit of refactoring. */
	usleep(200000);

	debug("PCM drained");
	return 0;
}

static int transport_acquire_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg, *rep;
	GUnixFDList *fd_list;
	GError *err = NULL;

	/* Check whether transport is already acquired - keep-alive mode. */
	if (t->bt_fd != -1) {
		debug("Reusing transport: %d", t->bt_fd);
		goto final;
	}

	msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path, BLUEZ_IFACE_MEDIA_TRANSPORT,
			t->state == TRANSPORT_PENDING ? "TryAcquire" : "Acquire");

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, &err);
		goto fail;
	}

	g_variant_get(g_dbus_message_get_body(rep), "(hqq)", (int32_t *)&t->bt_fd,
			(uint16_t *)&t->mtu_read, (uint16_t *)&t->mtu_write);

	fd_list = g_dbus_message_get_unix_fd_list(rep);
	t->bt_fd = g_unix_fd_list_get(fd_list, 0, &err);

	/* Minimize audio delay and increase responsiveness (seeking, stopping) by
	 * decreasing the BT socket output buffer. We will use a tripled write MTU
	 * value, in order to prevent tearing due to temporal heavy load. */
	size_t size = t->mtu_write * 3;
	if (setsockopt(t->bt_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == -1)
		warn("Couldn't set socket output buffer size: %s", strerror(errno));

	if (ioctl(t->bt_fd, TIOCOUTQ, &t->a2dp.bt_fd_coutq_init) == -1)
		warn("Couldn't get socket queued bytes: %s", strerror(errno));

	debug("New transport: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

fail:
	g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't acquire transport: %s", err->message);
		g_error_free(err);
	}

final:
	return t->bt_fd;
}

static int transport_release_bt_a2dp(struct ba_transport *t) {

	GDBusMessage *msg = NULL, *rep = NULL;
	GError *err = NULL;
	int ret = -1;

	/* If the transport has not been acquired, or it has been released already,
	 * there is no need to release it again. In fact, trying to release already
	 * closed transport will result in returning error message. */
	if (t->bt_fd == -1)
		return 0;

	debug("Releasing transport: %s", ba_transport_type_to_string(t->type));

	/* If the state is idle, it means that either transport was not acquired, or
	 * was released by the BlueZ. In both cases there is no point in a explicit
	 * release request. It might even return error (e.g. not authorized). */
	if (t->state != TRANSPORT_IDLE && t->dbus_owner != NULL) {

		msg = g_dbus_message_new_method_call(t->dbus_owner, t->dbus_path,
				BLUEZ_IFACE_MEDIA_TRANSPORT, "Release");

		if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
						G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, &err)) == NULL)
			goto fail;

		if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
			g_dbus_message_to_gerror(rep, &err);
			if (err->code == G_DBUS_ERROR_NO_REPLY ||
					err->code == G_DBUS_ERROR_SERVICE_UNKNOWN) {
				/* If BlueZ is already terminated (or is terminating), we won't receive
				 * any response. Do not treat such a case as an error - omit logging. */
				g_error_free(err);
				err = NULL;
			}
			else
				goto fail;
		}

	}

	debug("Closing BT: %d", t->bt_fd);

	ret = 0;
	close(t->bt_fd);
	t->bt_fd = -1;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	if (err != NULL) {
		error("Couldn't release transport: %s", err->message);
		g_error_free(err);
	}
	return ret;
}

static int transport_release_bt_rfcomm(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing RFCOMM: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	/* BlueZ does not trigger profile disconnection signal when the Bluetooth
	 * link has been lost (e.g. device power down). However, it is required to
	 * remove transport from the transport pool before reconnecting. */
	ba_transport_free(t);

	return 0;
}

static int transport_acquire_bt_sco(struct ba_transport *t) {

	struct hci_dev_info di;

	if (t->bt_fd != -1)
		return t->bt_fd;

	if (hci_devinfo(t->d->a->hci_dev_id, &di) == -1) {
		error("Couldn't get HCI device info: %s", strerror(errno));
		return -1;
	}

	if ((t->bt_fd = hci_open_sco(di.dev_id, &t->d->addr, t->type.codec != HFP_CODEC_CVSD)) == -1) {
		error("Couldn't open SCO link: %s", strerror(errno));
		return -1;
	}

	t->mtu_read = di.sco_mtu;
	t->mtu_write = di.sco_mtu;

	/* XXX: It seems, that the MTU values returned by the HCI interface
	 *      are incorrect (or our interpretation of them is incorrect). */
	t->mtu_read = 48;
	t->mtu_write = 48;

	debug("New SCO link: %d (MTU: R:%zu W:%zu)", t->bt_fd, t->mtu_read, t->mtu_write);

	return t->bt_fd;
}

static int transport_release_bt_sco(struct ba_transport *t) {

	if (t->bt_fd == -1)
		return 0;

	debug("Closing SCO: %d", t->bt_fd);

	shutdown(t->bt_fd, SHUT_RDWR);
	close(t->bt_fd);
	t->bt_fd = -1;

	return 0;
}

int transport_release_pcm(struct ba_pcm *pcm) {

	if (pcm->fd == -1)
		return 0;

	int oldstate;

	/* Transport IO workers are managed using thread cancellation mechanism,
	 * so we have to take into account a possibility of cancellation during the
	 * execution. In this release function it is important to perform actions
	 * atomically. Since close call is a cancellation point, it is required to
	 * temporally disable cancellation. For a better understanding of what is
	 * going on, see the io_thread_read_pcm() function. */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	debug("Closing PCM: %d", pcm->fd);
	close(pcm->fd);
	pcm->fd = -1;
	pcm->client = -1;

	pthread_setcancelstate(oldstate, NULL);
	return 0;
}

/**
 * Synchronous transport thread cancellation. */
void transport_pthread_cancel(pthread_t thread) {

	if (pthread_equal(thread, pthread_self()))
		return;
	if (pthread_equal(thread, config.main_thread))
		return;

	int err;
	if ((err = pthread_cancel(thread)) != 0)
		warn("Couldn't cancel transport thread: %s", strerror(err));
	if ((err = pthread_join(thread, NULL)) != 0)
		warn("Couldn't join transport thread: %s", strerror(err));
}

/**
 * Wrapper for release callback, which can be used by the pthread cleanup.
 *
 * This function CAN be used with transport_pthread_cleanup_lock() in order
 * to guard transport critical section during cleanup process. */
void transport_pthread_cleanup(struct ba_transport *t) {

	/* During the normal operation mode, the release callback should not
	 * be NULL. Hence, we will relay on this callback - file descriptors
	 * are closed in it. */
	if (t->release != NULL)
		t->release(t);

	/* Make sure, that after termination, this thread handler will not
	 * be used anymore. */
	t->thread = config.main_thread;

	transport_pthread_cleanup_unlock(t);

	/* XXX: If the order of the cleanup push is right, this function will
	 *      indicate the end of the IO/RFCOMM thread. */
	debug("Exiting IO thread: %s", ba_transport_type_to_string(t->type));
}

int transport_pthread_cleanup_lock(struct ba_transport *t) {
	int ret = pthread_mutex_lock(&t->mutex);
	t->cleanup_lock = true;
	return ret;
}

int transport_pthread_cleanup_unlock(struct ba_transport *t) {
	if (!t->cleanup_lock)
		return 0;
	t->cleanup_lock = false;
	return pthread_mutex_unlock(&t->mutex);
}
