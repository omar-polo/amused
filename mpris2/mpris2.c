/*
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <imsg.h>
#include <limits.h>
#include <locale.h>
#include <sha1.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#include <gio/gio.h>

#include "amused.h"
#include "log.h"
#include "playlist.h"
#include "spec.h"
#include "xmalloc.h"

#define NOTRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"

static void mpris_method_call(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GVariant *, GDBusMethodInvocation *, void *);
static GVariant *mpris_get_prop(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GError **, void *);
static gboolean mpris_set_prop(GDBusConnection *, const gchar *, const gchar *,
    const gchar *, const gchar *, GVariant *, GError **, void *);

static void mpris_player_method_call(GDBusConnection *, const gchar *,
    const gchar *, const gchar *, const gchar *, GVariant *,
    GDBusMethodInvocation *, void *);
static GVariant *mpris_player_get_prop(GDBusConnection *, const gchar *,
    const gchar *, const gchar *, const gchar *, GError **, void *);
static gboolean mpris_player_set_prop(GDBusConnection *, const gchar *,
    const gchar *, const gchar *, const gchar *, GVariant *, GError **, void *);

static gboolean imsg_dispatch(GIOChannel *, GIOCondition, gpointer);

static struct player_status	 status;
static char			 trackid[128] = NOTRACK;
struct imsgbuf			*imsgbuf;
static GDBusConnection		*global_conn;
static GDBusNodeInfo		*mpris_data, *mpris_player_data;

static const GDBusInterfaceVTable mpris_vtable = {
	mpris_method_call,
	mpris_get_prop,
	mpris_set_prop,
	{ 0 },
};

static const GDBusInterfaceVTable mpris_player_vtable = {
	mpris_player_method_call,
	mpris_player_get_prop,
	mpris_player_set_prop,
	{ 0 },
};

static const char *
loop_mode(void)
{
	if (status.mode.repeat_one)
		return "Track";
	if (status.mode.repeat_all)
		return "Playlist";
	return "None";
}

static const char *
playback_status(void)
{
	if (status.status == STATE_PLAYING)
		return "Playing";
	if (status.status == STATE_PAUSED)
		return "Paused";
	return "Stopped";
}

static const char *
base_name(const char *path)
{
	const char	*b;

	if ((b = strrchr(path, '/')) == NULL)
		return path;
	b++;
	if (*b == '\0')
		return path;
	return b;
}

static void
mpris_method_call(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *method_name, GVariant *params,
    GDBusMethodInvocation *invocation, void *data)
{
	log_warnx("called method %s from %s", method_name, sender);

	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
	    G_DBUS_ERROR_MATCH_RULE_NOT_FOUND, "un-implemented method");
}

static GVariant *
mpris_get_prop(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *prop_name, GError **err, void *data)
{
	if (!strcmp(prop_name, "CanQuit") ||
	    !strcmp(prop_name, "CanRaise") ||
	    !strcmp(prop_name, "CanSetFullscreen") ||
	    !strcmp(prop_name, "Fullscreen") ||
	    !strcmp(prop_name, "Identity"))
		return g_variant_new_boolean(0);

	if (!strcmp(prop_name, "DesktopEntry"))
		return g_variant_new_string("amused");

	/* we don't implement the TrackList interface */
	if (!strcmp(prop_name, "HasTrackList"))
		return g_variant_new_boolean(0);

	if (!strcmp(prop_name, "SupportedMimeTypes")) {
		GVariant *res[4];

		if ((res[0] = g_variant_new_string("audio/flac")) == NULL)
			fatal("g_variant_new_string");
		if ((res[1] = g_variant_new_string("audio/mpeg")) == NULL)
			fatal("g_variant_new_string");
		if ((res[2] = g_variant_new_string("audio/opus")) == NULL)
			fatal("g_variant_new_string");
		if ((res[3] = g_variant_new_string("audio/vorbis")) == NULL)
			fatal("g_variant_new_string");
		return g_variant_new_array(NULL, res, 4);
	}

	if (!strcmp(prop_name, "SupportedUriSchemes")) {
		GVariant *res[1];

		if ((res[0] = g_variant_new_string("file")) == NULL)
			fatal("g_variant_new_string");
		return g_variant_new_array(NULL, res, 1);
	}

	log_warnx("%s: unknown property: %s", __func__, prop_name);
	return NULL;
}

static gboolean
mpris_set_prop(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *prop_name, GVariant *value, GError **err, void *data)
{
	log_warnx("trying to set property %s from %s", prop_name, sender);
	g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
	    "Property %s not supported.", prop_name);
	return FALSE;
}

static void
mpris_player_method_call(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *method_name, GVariant *params,
    GDBusMethodInvocation *invocation, void *data)
{
	struct player_seek	 seek;
	GVariant		*tid, *offset;
	const char		*tids;

	log_warnx("called method %s from %s", method_name, sender);

	if (!strcmp(method_name, "Next")) {
		imsg_compose(imsgbuf, IMSG_CTL_NEXT, 0, 0, -1, NULL, 0);
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "Pause")) {
		imsg_compose(imsgbuf, IMSG_CTL_PAUSE, 0, 0, -1, NULL, 0);
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "Play")) {
		imsg_compose(imsgbuf, IMSG_CTL_PLAY, 0, 0, -1, NULL, 0);
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "PlayPause")) {
		imsg_compose(imsgbuf, IMSG_CTL_TOGGLE_PLAY, 0, 0, -1, NULL, 0);
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "Previous")) {
		imsg_compose(imsgbuf, IMSG_CTL_PREV, 0, 0, -1, NULL, 0);
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "Seek")) {
		offset = g_variant_get_child_value(params, 0);

		memset(&seek, 0, sizeof(seek));
		seek.offset = g_variant_get_int64(offset) / 1000000L;
		seek.relative = 1;
		imsg_compose(imsgbuf, IMSG_CTL_SEEK, 0, 0, -1,
		    &seek, sizeof(seek));
		imsg_flush(imsgbuf);
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	if (!strcmp(method_name, "SetPosition")) {
		tid = g_variant_get_child_value(params, 0);
		offset = g_variant_get_child_value(params, 1);

		tids = g_variant_get_string(tid, NULL);
		if (!strcmp(tids, NOTRACK) || strcmp(tids, trackid) != 0) {
			g_dbus_method_invocation_return_value(invocation, NULL);
			return;
		}

		memset(&seek, 0, sizeof(seek));
		seek.offset = g_variant_get_int64(offset) / 1000000L;
		imsg_compose(imsgbuf, IMSG_CTL_SEEK, 0, 0, -1,
		    &seek, sizeof(seek));
		imsg_flush(imsgbuf);

		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	/* OpenUri could be implemented but it's not atm */

	g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
	    G_DBUS_ERROR_MATCH_RULE_NOT_FOUND, "un-implemented method");
}

static GVariant *
mpris_player_get_prop(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *prop_name, GError **err, void *data)
{
	if (!strcmp(prop_name, "CanControl") ||
	    !strcmp(prop_name, "CanGoNext") ||
	    !strcmp(prop_name, "CanGoPrevious") ||
	    !strcmp(prop_name, "CanPause") ||
	    !strcmp(prop_name, "CanPlay") ||
	    !strcmp(prop_name, "CanSeek"))
		return g_variant_new_boolean(1);

	if (!strcmp(prop_name, "LoopStatus"))
		return g_variant_new_string(loop_mode());

	/* we don't support different rates */
	if (!strcmp(prop_name, "MaximumRate") ||
	    !strcmp(prop_name, "MinimumRate"))
		return g_variant_new_double(1.0);

	if (!strcmp(prop_name, "Metadata")) {
		GVariantBuilder builder;

		g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
		g_variant_builder_add(&builder, "{sv}", "mpris:trackid",
		    g_variant_new_object_path(trackid));
		g_variant_builder_add(&builder, "{sv}", "mpris:length",
		    g_variant_new_int64(status.duration * 1000000L));
		g_variant_builder_add(&builder, "{sv}", "xesam:title",
		    g_variant_new_string(base_name(status.path)));

		return g_variant_builder_end(&builder);
	}

	if (!strcmp(prop_name, "PlaybackStatus"))
		return g_variant_new_string(playback_status());

	if (!strcmp(prop_name, "Position")) /* microseconds... */
		return g_variant_new_int64(status.position * 1000000);

	if (!strcmp(prop_name, "Rate"))
		return g_variant_new_double(1.0);

	if (!strcmp(prop_name, "Shuffle"))
		return g_variant_new_boolean(0);

	if (!strcmp(prop_name, "Volume"))
		return g_variant_new_double(1.0);

	log_warnx("trying to get an unknown property %s", prop_name);

	return NULL;
}

static gboolean
mpris_player_set_prop(GDBusConnection *conn, const gchar *sender,
    const gchar *obj_path, const gchar *interface_name,
    const gchar *prop_name, GVariant *value, GError **err, void *data)
{
	const char *str;
	struct player_mode mode;

	if (!strcmp(prop_name, "LoopStatus")) {
		str = g_variant_get_string(value, NULL);

		mode.repeat_one = MODE_UNDEF;
		mode.repeat_all = MODE_UNDEF;
		mode.consume = MODE_UNDEF;

		if (!strcmp(str, "None")) {
			mode.repeat_one = MODE_OFF;
			mode.repeat_all = MODE_OFF;
			mode.consume = MODE_OFF;
		}
		if (!strcmp(str, "Track"))
			mode.repeat_one = MODE_ON;
		if (!strcmp(str, "Playlist"))
			mode.repeat_all = MODE_ON;

		imsg_compose(imsgbuf, IMSG_CTL_MODE, 0, 0, -1,
		    &mode, sizeof(mode));
		imsg_flush(imsgbuf);
		return TRUE;
	}

	g_set_error(err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
	    "Property %s not supported.", prop_name);
	return FALSE;
}

static void
on_bus_acquired(GDBusConnection *conn, const gchar *name,
    gpointer user_data)
{
	guint		reg_id;

	reg_id = g_dbus_connection_register_object(conn,
	    "/org/mpris/MediaPlayer2", mpris_data->interfaces[0],
	    &mpris_vtable, NULL, NULL, NULL);
	if (reg_id == 0)
		fatalx("failed to register MediaPlayer2 object");

	reg_id = g_dbus_connection_register_object(conn,
	    "/org/mpris/MediaPlayer2", mpris_player_data->interfaces[0],
	    &mpris_player_vtable, NULL, NULL, NULL);
	if (reg_id == 0)
		fatalx("failed to register MediaPlayer2 object");

	global_conn = conn;

	imsg_compose(imsgbuf, IMSG_CTL_STATUS, 0, 0, -1, NULL, 0);
	imsg_compose(imsgbuf, IMSG_CTL_MONITOR, 0, 0, -1, NULL, 0);
	imsg_flush(imsgbuf);
}

static void
on_name_acquired(GDBusConnection *conn, const gchar *name,
    gpointer user_data)
{
	log_info("Acquired the name %s on the session bus", name);
}

static void
on_name_lost(GDBusConnection *conn, const gchar *name,
    gpointer user_data)
{
	log_info("Lost the name %s on the session bus", name);
	exit(1);
}

static void
property_changed(const char *prop_name, GVariant *value)
{
	GVariantBuilder builder;
	GVariantBuilder invalidated_builder;
	GVariant *v;
	GError *err = NULL;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_ARRAY);
	g_variant_builder_init(&invalidated_builder, G_VARIANT_TYPE("as"));

	g_variant_builder_add(&builder, "{sv}", prop_name, value);
	v = g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2", &builder,
	    &invalidated_builder);
	if (v == NULL)
		fatalx("g_variant_builder_add");

	g_dbus_connection_emit_signal(global_conn,
	    NULL /* "org.mpris.MediaPlayer2.amused" */, "/org/mpris/MediaPlayer2",
	    "org.freedesktop.DBus.Properties", "PropertiesChanged", v,
	    &err);
	return;
}

static void
property_invalidated(const char *prop_name)
{
	GVariantBuilder builder;
	GVariantBuilder invalidated_builder;
	GVariant *v;
	GError *err = NULL;

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_init(&invalidated_builder, G_VARIANT_TYPE("as"));

	g_variant_builder_add(&invalidated_builder, "s", prop_name);

	v = g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2", &builder,
	    &invalidated_builder);
	if (v == NULL)
		fatalx("g_variant_builder_add");

	g_dbus_connection_emit_signal(global_conn,
	    NULL/* "org.mpris.MediaPlayer2.amused" */, "/org/mpris/MediaPlayer2",
	    "org.freedesktop.DBus.Properties", "PropertiesChanged", v,
	    &err);
	return;
}

static gboolean
imsg_dispatch(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	struct imsg		 imsg;
	struct ibuf		 ibuf;
	struct player_event	 event;
	ssize_t			 n;
	size_t			 datalen;
	const char		*msg;
	char			 sha1buf[SHA1_DIGEST_STRING_LENGTH];

	if ((n = imsg_read(imsgbuf)) == -1) {
		if (errno == EAGAIN)
			return TRUE;
		fatal("imsg_read");
	}

	for (;;) {
		if ((n = imsg_get(imsgbuf, &imsg)) == -1)
			fatal("imsg_get");
		if (n == 0)
			break;

		switch (imsg_get_type(&imsg)) {
		case IMSG_CTL_ERR:
			if (imsg_get_ibuf(&imsg, &ibuf) == -1 ||
			    (datalen = ibuf_size(&ibuf)) == 0 ||
			    (msg = ibuf_data(&ibuf)) == NULL ||
			    msg[datalen - 1] != '\0')
				fatalx("malformed error message");
			log_warnx("error: %s", msg);
			break;

		case IMSG_CTL_MONITOR:
			if (imsg_get_data(&imsg, &event, sizeof(event)) == -1)
				fatalx("corrupted IMSG_CTL_MONITOR");
			switch (event.event) {
			case IMSG_CTL_PLAY:
			case IMSG_CTL_PAUSE:
			case IMSG_CTL_STOP:
			case IMSG_CTL_NEXT:
			case IMSG_CTL_PREV:
			case IMSG_CTL_JUMP:
				imsg_compose(imsgbuf, IMSG_CTL_STATUS, 0, 0,
				    -1, NULL, 0);
				imsg_flush(imsgbuf);
				break;

			case IMSG_CTL_MODE:
				memcpy(&status.mode, &event.mode,
				    sizeof(status.mode));
				property_changed("LoopStatus",
				    g_variant_new_string(loop_mode()));
				break;

			case IMSG_CTL_SEEK:
				if (labs(status.position - event.position) >= 3)
					property_invalidated("Position");
				if (status.duration != event.duration)
					property_invalidated("Metadata");
				status.position = event.position;
				status.duration = event.duration;
				break;

			default:
				log_debug("ignoring event %d", event.event);
				break;
			}
			break;

		case IMSG_CTL_STATUS:
			if (imsg_get_data(&imsg, &status, sizeof(status)) == -1)
				fatalx("corrupted IMSG_CTL_STATUS");
			if (status.path[sizeof(status.path)-1]
			    != '\0')
				fatalx("corrupted IMSG_CTL_STATUS path");
			if (status.path[0] == '\0')
				strlcpy(trackid, NOTRACK, sizeof(trackid));
			else {
				SHA1Data(status.path, strlen(status.path),
				    sha1buf);
				snprintf(trackid, sizeof(trackid),
				    "/com/omarpolo/Amused/Track/%s", sha1buf);
			}
			// XXX notify the change in at least the play
			// status (paused, play, ...)
			property_invalidated("Metadata");
			break;

		default:
			log_debug("ignoring imsg %d", imsg_get_type(&imsg));
		}

		imsg_free(&imsg);
	}

	return (TRUE);
}

void __dead
usage(void)
{
	fprintf(stderr, "usage: %s [-dv] [-s sock]\n",
	    getprogname());
	exit(1);
}

int
main(int argc, char **argv)
{
	struct sockaddr_un	 sa;
	char			*sock = NULL;
	int			 fd;
	int			 ch;
	int			 debug = 0, verbose = 0;
	guint			 owner_id;
	GMainLoop		*loop;
	GIOChannel		*sock_chan;

	setlocale(LC_ALL, NULL);

	log_init(1, LOG_DAEMON);

	while ((ch = getopt(argc, argv, "ds:v")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 's':
			sock = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			usage();
			break;
		}
	}
	argc -= optind;
	argv -= optind;
	if (argc != 0)
		usage();

	if (!debug && daemon(1, 0) == -1)
		fatal("daemon");

	log_init(debug, LOG_DAEMON);
	log_setverbose(verbose);
	log_procinit("mpris2");

	if (sock == NULL) {
		const char *tmpdir;

		if ((tmpdir = getenv("TMPDIR")) == NULL)
			tmpdir = "/tmp";

		xasprintf(&sock, "%s/amused-%d", tmpdir, getuid());
	}

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, sock, sizeof(sa.sun_path))
	    >= sizeof(sa.sun_path))
		fatalx("socket path too long: %s", sa.sun_path);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		fatal("socket");
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == -1)
		fatal("connect");

	mpris_data = g_dbus_node_info_new_for_xml(mpris_xml, NULL);
	if (mpris_data == NULL)
		fatalx("g_dbus_node_info_new_for_xml(mpris_xml) failed");

	mpris_player_data = g_dbus_node_info_new_for_xml(mpris_player_xml,
	    NULL);
	if (mpris_player_data == NULL)
		fatalx("g_dbus_node_info_new_for_xml(mpris_player) failed");

	owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
	    "org.mpris.MediaPlayer2.amused", G_BUS_NAME_OWNER_FLAGS_REPLACE,
	    on_bus_acquired, on_name_acquired, on_name_lost, NULL, NULL);

	loop = g_main_loop_new(NULL, FALSE);
	if (loop == NULL)
		fatal("g_main_loop_new");

	if ((sock_chan = g_io_channel_unix_new(fd)) == NULL)
		fatal("g_io_channel_unix_new");

	imsgbuf = xmalloc(sizeof(*imsgbuf));
	imsg_init(imsgbuf, fd);

	g_io_add_watch(sock_chan, G_IO_IN, imsg_dispatch, NULL);

	g_main_loop_run(loop);
	g_bus_unown_name(owner_id);

	return (0);
}
