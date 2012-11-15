/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "log.h"
#include "../src/adapter.h"
#include "../src/device.h"

#include "error.h"
#include "dbus-common.h"
#include "device.h"
#include "avdtp.h"
#include "control.h"
#include "avctp.h"
#include "avrcp.h"
#include "sink.h"
#include "source.h"

#define AUDIO_INTERFACE "org.bluez.Audio"

#define CONTROL_CONNECT_TIMEOUT 2
#define AVDTP_CONNECT_TIMEOUT 1
#define AVDTP_CONNECT_TIMEOUT_BOOST 1
#define HEADSET_CONNECT_TIMEOUT 1

typedef enum {
	AUDIO_STATE_DISCONNECTED,
	AUDIO_STATE_CONNECTING,
	AUDIO_STATE_CONNECTED,
} audio_state_t;

struct dev_priv {
	audio_state_t state;

	sink_state_t sink_state;
	avctp_state_t avctp_state;

	DBusMessage *conn_req;
	DBusMessage *dc_req;

	guint control_timer;
	guint avdtp_timer;
	guint headset_timer;
	guint dc_id;

	gboolean disconnecting;
};

static unsigned int sink_callback_id = 0;
static unsigned int avctp_callback_id = 0;
static unsigned int avdtp_callback_id = 0;

static void device_free(struct audio_device *dev)
{
	struct dev_priv *priv = dev->priv;

	if (priv) {
		if (priv->control_timer)
			g_source_remove(priv->control_timer);
		if (priv->avdtp_timer)
			g_source_remove(priv->avdtp_timer);
		if (priv->headset_timer)
			g_source_remove(priv->headset_timer);
		if (priv->dc_req)
			dbus_message_unref(priv->dc_req);
		if (priv->conn_req)
			dbus_message_unref(priv->conn_req);
		if (priv->dc_id)
			device_remove_disconnect_watch(dev->btd_dev,
							priv->dc_id);
		g_free(priv);
	}

	btd_device_unref(dev->btd_dev);

	g_free(dev);
}

static const char *state2str(audio_state_t state)
{
	switch (state) {
	case AUDIO_STATE_DISCONNECTED:
		return "disconnected";
	case AUDIO_STATE_CONNECTING:
		return "connecting";
	case AUDIO_STATE_CONNECTED:
		return "connected";
	default:
		error("Invalid audio state %d", state);
		return NULL;
	}
}

static gboolean control_connect_timeout(gpointer user_data)
{
	struct audio_device *dev = user_data;

	dev->priv->control_timer = 0;

	if (dev->control)
		avrcp_connect(dev);

	return FALSE;
}

static gboolean device_set_control_timer(struct audio_device *dev)
{
	struct dev_priv *priv = dev->priv;

	if (!dev->control)
		return FALSE;

	if (priv->control_timer)
		return FALSE;

	priv->control_timer = g_timeout_add_seconds(CONTROL_CONNECT_TIMEOUT,
							control_connect_timeout,
							dev);

	return TRUE;
}

static void device_remove_control_timer(struct audio_device *dev)
{
	if (dev->priv->control_timer)
		g_source_remove(dev->priv->control_timer);
	dev->priv->control_timer = 0;
}

static void device_remove_avdtp_timer(struct audio_device *dev)
{
	if (dev->priv->avdtp_timer)
		g_source_remove(dev->priv->avdtp_timer);
	dev->priv->avdtp_timer = 0;
}

static void device_remove_headset_timer(struct audio_device *dev)
{
	if (dev->priv->headset_timer)
		g_source_remove(dev->priv->headset_timer);
	dev->priv->headset_timer = 0;
}

static void disconnect_cb(struct btd_device *btd_dev, gboolean removal,
				void *user_data)
{
	struct audio_device *dev = user_data;
	struct dev_priv *priv = dev->priv;

	if (priv->state == AUDIO_STATE_DISCONNECTED)
		return;

	if (priv->disconnecting)
		return;

	priv->disconnecting = TRUE;

	device_remove_control_timer(dev);
	device_remove_avdtp_timer(dev);
	device_remove_headset_timer(dev);

	if (dev->control)
		avrcp_disconnect(dev);

	if (dev->sink && priv->sink_state != SINK_STATE_DISCONNECTED)
		sink_disconnect(dev, TRUE);
	else
		priv->disconnecting = FALSE;
}

static void device_set_state(struct audio_device *dev, audio_state_t new_state)
{
	DBusConnection *conn = btd_get_dbus_connection();
	struct dev_priv *priv = dev->priv;
	const char *state_str;
	DBusMessage *reply = NULL;

	state_str = state2str(new_state);
	if (!state_str)
		return;

	if (new_state == AUDIO_STATE_DISCONNECTED) {
		if (priv->dc_id) {
			device_remove_disconnect_watch(dev->btd_dev,
							priv->dc_id);
			priv->dc_id = 0;
		}
	} else if (new_state == AUDIO_STATE_CONNECTING)
		priv->dc_id = device_add_disconnect_watch(dev->btd_dev,
						disconnect_cb, dev, NULL);

	if (dev->priv->state == new_state) {
		DBG("state change attempted from %s to %s",
							state_str, state_str);
		return;
	}

	dev->priv->state = new_state;

	if (new_state == AUDIO_STATE_DISCONNECTED) {
		if (priv->dc_req) {
			reply = dbus_message_new_method_return(priv->dc_req);
			dbus_message_unref(priv->dc_req);
			priv->dc_req = NULL;
			g_dbus_send_message(conn, reply);
		}
		priv->disconnecting = FALSE;
	}

	if (priv->conn_req && new_state != AUDIO_STATE_CONNECTING) {
		if (new_state == AUDIO_STATE_CONNECTED)
			reply = dbus_message_new_method_return(priv->conn_req);
		else
			reply = btd_error_failed(priv->conn_req,
							"Connect Failed");

		dbus_message_unref(priv->conn_req);
		priv->conn_req = NULL;
		g_dbus_send_message(conn, reply);
	}

	emit_property_changed(device_get_path(dev->btd_dev),
				AUDIO_INTERFACE, "State",
				DBUS_TYPE_STRING, &state_str);
}

static void device_avdtp_cb(struct audio_device *dev, struct avdtp *session,
				avdtp_session_state_t old_state,
				avdtp_session_state_t new_state,
				void *user_data)
{
	if (!dev->control)
		return;

	if (new_state == AVDTP_SESSION_STATE_CONNECTED) {
		if (avdtp_stream_setup_active(session))
			device_set_control_timer(dev);
		else
			avrcp_connect(dev);
	}
}

static void device_sink_cb(struct audio_device *dev,
				sink_state_t old_state,
				sink_state_t new_state,
				void *user_data)
{
	struct dev_priv *priv = dev->priv;

	if (!dev->sink)
		return;

	priv->sink_state = new_state;

	switch (new_state) {
	case SINK_STATE_DISCONNECTED:
		if (dev->control) {
			device_remove_control_timer(dev);
			avrcp_disconnect(dev);
		}

		device_set_state(dev, AUDIO_STATE_DISCONNECTED);
		break;
	case SINK_STATE_CONNECTING:
		device_remove_avdtp_timer(dev);
		device_set_state(dev, AUDIO_STATE_CONNECTING);
		break;
	case SINK_STATE_CONNECTED:
		if (old_state == SINK_STATE_PLAYING)
			break;
		device_set_state(dev, AUDIO_STATE_CONNECTED);
		break;
	case SINK_STATE_PLAYING:
		break;
	}
}

static void device_avctp_cb(struct audio_device *dev,
				avctp_state_t old_state,
				avctp_state_t new_state,
				void *user_data)
{
	if (!dev->control)
		return;

	dev->priv->avctp_state = new_state;

	switch (new_state) {
	case AVCTP_STATE_DISCONNECTED:
		break;
	case AVCTP_STATE_CONNECTING:
		device_remove_control_timer(dev);
		break;
	case AVCTP_STATE_CONNECTED:
		break;
	}
}

static DBusMessage *dev_connect(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *dev = data;
	struct dev_priv *priv = dev->priv;

	if (priv->state == AUDIO_STATE_CONNECTING)
		return btd_error_in_progress(msg);
	else if (priv->state == AUDIO_STATE_CONNECTED)
		return btd_error_already_connected(msg);

	dev->auto_connect = TRUE;

	if (priv->state != AUDIO_STATE_CONNECTING && dev->sink) {
		struct avdtp *session = avdtp_get(&dev->src, &dev->dst);

		if (!session)
			return btd_error_failed(msg,
					"Failed to get AVDTP session");

		sink_setup_stream(dev->sink, session);
		avdtp_unref(session);
	}

	/* The previous calls should cause a call to the state callback to
	 * indicate AUDIO_STATE_CONNECTING */
	if (priv->state != AUDIO_STATE_CONNECTING)
		return btd_error_failed(msg, "Connect Failed");

	priv->conn_req = dbus_message_ref(msg);

	return NULL;
}

static DBusMessage *dev_disconnect(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *dev = data;
	struct dev_priv *priv = dev->priv;

	if (priv->state == AUDIO_STATE_DISCONNECTED)
		return btd_error_not_connected(msg);

	if (priv->dc_req)
		return dbus_message_new_method_return(msg);

	priv->dc_req = dbus_message_ref(msg);

	if (dev->control) {
		device_remove_control_timer(dev);
		avrcp_disconnect(dev);
	}

	if (dev->sink && priv->sink_state != SINK_STATE_DISCONNECTED)
		sink_disconnect(dev, TRUE);
	else {
		dbus_message_unref(priv->dc_req);
		priv->dc_req = NULL;
		return dbus_message_new_method_return(msg);
	}

	return NULL;
}

static DBusMessage *dev_get_properties(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct audio_device *device = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *state;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* State */
	state = state2str(device->priv->state);
	if (state)
		dict_append_entry(&dict, "State", DBUS_TYPE_STRING, &state);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static const GDBusMethodTable dev_methods[] = {
	{ GDBUS_ASYNC_METHOD("Connect", NULL, NULL, dev_connect) },
	{ GDBUS_METHOD("Disconnect", NULL, NULL, dev_disconnect) },
	{ GDBUS_METHOD("GetProperties",
		NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
		dev_get_properties) },
	{ }
};

static const GDBusSignalTable dev_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

struct audio_device *audio_device_register(struct btd_device *device,
							const bdaddr_t *src,
							const bdaddr_t *dst)
{
	struct audio_device *dev;

	dev = g_new0(struct audio_device, 1);

	dev->btd_dev = btd_device_ref(device);
	bacpy(&dev->dst, dst);
	bacpy(&dev->src, src);
	dev->priv = g_new0(struct dev_priv, 1);
	dev->priv->state = AUDIO_STATE_DISCONNECTED;

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					device_get_path(dev->btd_dev),
					AUDIO_INTERFACE, dev_methods,
					dev_signals, NULL, dev, NULL)) {
		error("Unable to register %s on %s", AUDIO_INTERFACE,
						device_get_path(dev->btd_dev));
		device_free(dev);
		return NULL;
	}

	DBG("Registered interface %s on path %s", AUDIO_INTERFACE,
						device_get_path(dev->btd_dev));

	if (sink_callback_id == 0)
		sink_callback_id = sink_add_state_cb(device_sink_cb, NULL);

	if (avdtp_callback_id == 0)
		avdtp_callback_id = avdtp_add_state_cb(device_avdtp_cb, NULL);
	if (avctp_callback_id == 0)
		avctp_callback_id = avctp_add_state_cb(device_avctp_cb, NULL);

	return dev;
}

gboolean audio_device_is_active(struct audio_device *dev,
						const char *interface)
{
	if (!interface) {
		if ((dev->sink || dev->source) &&
				avdtp_is_connected(&dev->src, &dev->dst))
			return TRUE;
	} else if (!strcmp(interface, AUDIO_SINK_INTERFACE) && dev->sink &&
				avdtp_is_connected(&dev->src, &dev->dst))
		return TRUE;
	else if (!strcmp(interface, AUDIO_SOURCE_INTERFACE) && dev->source &&
				avdtp_is_connected(&dev->src, &dev->dst))
		return TRUE;
	else if (!strcmp(interface, AUDIO_CONTROL_INTERFACE) && dev->control &&
				control_is_active(dev))
		return TRUE;

	return FALSE;
}

void audio_device_unregister(struct audio_device *device)
{
	if (device->hs_preauth_id) {
		g_source_remove(device->hs_preauth_id);
		device->hs_preauth_id = 0;
	}

	if (device->sink)
		sink_unregister(device);

	if (device->source)
		source_unregister(device);

	if (device->control)
		control_unregister(device);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
					device_get_path(device->btd_dev),
					AUDIO_INTERFACE);

	device_free(device);
}
