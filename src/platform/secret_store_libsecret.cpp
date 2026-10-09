/*
 * libsecret BEFORE Qt, and that order is load-bearing. GLib uses
 * "signals" as an ordinary identifier, and Qt's moc keywords define it as
 * a macro -- whichever is parsed second loses. Including glib first lets
 * its headers see their own identifier, and Qt's macros then apply only to
 * this project's own declarations.
 */
#include <libsecret/secret.h>

#include "platform/secret_store.h"

/*
 * The Secret Service backend (project.md sec 11): GNOME Keyring, KWallet
 * and anything else speaking the freedesktop Secret Service API, reached
 * through libsecret.
 *
 * Preferred over the passphrase file wherever it works, because the
 * keyring is already unlocked by the login session -- the user types
 * nothing, and the secret is protected by the same thing protecting the
 * rest of their session rather than by a second passphrase they have to
 * invent and remember.
 *
 * It is NOT always available, which is why the file backend exists and why
 * the choice is made at runtime: a headless login, a container, a minimal
 * window manager and a broken D-Bus session all reach this code on a
 * machine where libsecret is installed and no service answers.
 */

namespace {

/*
 * The attribute schema. "id" is the profile-scoped key the rest of the app
 * uses; nothing else is stored, and in particular no hostname or username
 * -- keyring attributes are searchable metadata, not secrets, and putting
 * infrastructure in them would leak through a channel the encryption does
 * not cover.
 */
const SecretSchema *bssh_schema() {
	static const SecretSchema schema = {
		"se.vibes.beerssh.Secret",
		SECRET_SCHEMA_NONE,
		{
			{"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
			{nullptr, SecretSchemaAttributeType(0)}
		},
		0, 0, 0, 0, 0, 0, 0, 0
	};
	return &schema;
}

} /* namespace */

class bssh_secret_store_libsecret : public bssh_secret_store {
public:
	QString backend_name() const override {
		return QStringLiteral("system keyring (Secret Service)");
	}

	/*
	 * The keyring may well be backed by a TPM, but libsecret does not say
	 * so and guessing would be the sort of reassurance sec 11 warns
	 * against. Reported as not hardware-backed: understating it is the
	 * safe direction to be wrong in.
	 */
	bool is_hardware_backed() const override { return false; }

	/* Unlocked by the login session; nothing for the app to ask. */
	bool is_unlocked() const override { return true; }
	bool needs_passphrase() const override { return false; }

	bool unlock(const QString &passphrase) override {
		Q_UNUSED(passphrase);
		return true;
	}

	void lock() override {
		/*
		 * Deliberately does nothing. The keyring's lock state belongs to
		 * the session, and an application locking it would affect every
		 * other program using it.
		 */
	}

	bool store(const QString &id, const QString &secret) override {
		GError *error = nullptr;
		const QByteArray id_utf8 = id.toUtf8();
		const QByteArray secret_utf8 = secret.toUtf8();
		const QByteArray label = QStringLiteral("BeerSSH: %1").arg(id).toUtf8();

		const gboolean ok = secret_password_store_sync(
		        bssh_schema(), SECRET_COLLECTION_DEFAULT, label.constData(),
		        secret_utf8.constData(), nullptr, &error,
		        "id", id_utf8.constData(), nullptr);

		if (error) {
			g_error_free(error);
			return false;
		}
		return ok;
	}

	QString load(const QString &id) const override {
		GError *error = nullptr;
		const QByteArray id_utf8 = id.toUtf8();

		gchar *value = secret_password_lookup_sync(
		        bssh_schema(), nullptr, &error, "id", id_utf8.constData(), nullptr);

		if (error) {
			g_error_free(error);
			return QString();
		}
		if (!value) return QString();

		const QString secret = QString::fromUtf8(value);

		/* Frees with the allocator libsecret used, and scrubs on the way. */
		secret_password_free(value);
		return secret;
	}

	bool erase(const QString &id) override {
		GError *error = nullptr;
		const QByteArray id_utf8 = id.toUtf8();

		secret_password_clear_sync(bssh_schema(), nullptr, &error,
		                            "id", id_utf8.constData(), nullptr);

		if (error) {
			g_error_free(error);
			return false;
		}

		/*
		 * The return value is dropped on purpose: it answers a different
		 * question from the one this method asks.
		 * secret_password_clear_sync() reports whether anything MATCHED,
		 * so it hands back FALSE with no GError when the keyring held no
		 * such entry -- which is "already gone", the state the caller
		 * wanted, not a failure to reach it. Only an error is a failure,
		 * and an error sets `error`.
		 *
		 * Returning that FALSE cost more than a wrong word.
		 * bssh_profile_store::remove() files a key on the orphan list
		 * when erase() fails, and bssh_sweep_orphaned_secrets() drops
		 * only the keys whose erase SUCCEEDS. So deleting a host whose
		 * entry was already gone added a key that every later sweep
		 * re-erased, failed on, and wrote back: the list in app.ini grew
		 * for the life of the install and never shrank, costing a D-Bus
		 * round trip per stale entry for ever.
		 *
		 * Both sibling backends already answer this way, which is the
		 * asymmetry that gave it away: the keystore returns true outright
		 * for a key it does not hold, and the file store's
		 * remove-then-flush cannot tell the two cases apart.
		 */
		return true;
	}
};

namespace {

/*
 * Whether a Secret Service answers, asked so that the answer comes in a few
 * seconds whatever the session is like (project.md sec 11, sec 8.4's
 * finding of 2026-10-09).
 *
 * secret_service_get_sync() alone waits as long as D-Bus does -- its
 * default is 25 seconds -- and it was asked while the main window was
 * being built: on a display with no session bus the first window took 27
 * seconds, measured, and as long again with a bus whose secrets service
 * was activatable and never came up. Two things made it that slow, and
 * each is closed here:
 *
 * - With no bus address, GLib AUTOLAUNCHES a bus through dbus-launch, a
 *   daemon nothing reaps. Only an address or the user's runtime bus
 *   socket is used, so an absent bus is an immediate "no".
 * - An activatable service that never starts was waited for in full. The
 *   bus is asked first whether the name is owned, in one second; if not,
 *   the service is started with three seconds to answer.
 */
bool secret_service_answers() {
	const char *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!address || !*address) {
		gchar *runtime_bus = g_build_filename(g_get_user_runtime_dir(), "bus", nullptr);
		const bool there = g_file_test(runtime_bus, G_FILE_TEST_EXISTS);
		g_free(runtime_bus);
		if (!there) return false;
	}

	GError *error = nullptr;
	GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (!bus) {
		g_clear_error(&error);
		return false;
	}

	bool answers = false;
	GVariant *owned = g_dbus_connection_call_sync(
	        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
	        "NameHasOwner", g_variant_new("(s)", "org.freedesktop.secrets"),
	        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
	if (owned) {
		gboolean has = FALSE;
		g_variant_get(owned, "(b)", &has);
		g_variant_unref(owned);
		answers = has;

		if (!answers) {
			GVariant *started = g_dbus_connection_call_sync(
			        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
			        "StartServiceByName", g_variant_new("(su)", "org.freedesktop.secrets", 0u),
			        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &error);
			if (started) {
				answers = true;
				g_variant_unref(started);
			}
		}
	}
	g_clear_error(&error);
	g_object_unref(bus);
	return answers;
}

} /* namespace */

bssh_secret_store *bssh_make_libsecret_store() {
	/*
	 * Asked, not assumed. libsecret being installed says nothing about a
	 * service answering: this returns null on a headless login, in a
	 * container, or with no session bus, and the caller falls back to the
	 * file store rather than failing to save anything. Asked BOUNDED first,
	 * so that by the time libsecret is called a service is known to answer.
	 */
	if (!secret_service_answers()) return nullptr;

	GError *error = nullptr;
	SecretService *service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);

	if (error) {
		g_error_free(error);
		return nullptr;
	}
	if (!service) return nullptr;

	g_object_unref(service);

	static bssh_secret_store_libsecret store;
	return &store;
}
