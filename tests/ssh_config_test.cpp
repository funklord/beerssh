#include <QtTest>

#include "ssh_config_test.h"
#include "model/ssh_config.h"

#include "ssh/jump_tunnel.h"
#include "model/profile_store.h"
#include "ui/ssh_config_import.h"

#include <QListWidget>
#include <QTemporaryDir>

namespace {

/* The parsed host with this alias, or a default-constructed one. */
bssh_ssh_config_host_t find(const QList<bssh_ssh_config_host_t> &hosts,
	                           const QString &alias) {
	for (const bssh_ssh_config_host_t &host : hosts) {
		if (host.alias == alias) return host;
	}
	return bssh_ssh_config_host_t();
}

QStringList aliases_of(const QList<bssh_ssh_config_host_t> &hosts) {
	QStringList out;
	for (const bssh_ssh_config_host_t &host : hosts) out.append(host.alias);
	return out;
}

} /* namespace */

void ssh_config_test::a_plain_host_block_becomes_a_profile() {
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host build\n"
	        "    HostName build.example.com\n"
	        "    User nabbe\n"
	        "    Port 2222\n"));

	QCOMPARE(hosts.size(), 1);
	QCOMPARE(hosts.at(0).alias, QStringLiteral("build"));
	QCOMPARE(hosts.at(0).hostname, QStringLiteral("build.example.com"));
	QCOMPARE(hosts.at(0).user, QStringLiteral("nabbe"));
	QCOMPARE(int(hosts.at(0).port), 2222);

	/* The profile keeps the alias as its NAME -- that is what the user
	 * types after `ssh` and therefore what they will look for in a list. */
	const bssh_profile_t profile = hosts.at(0).to_profile();
	QCOMPARE(profile.name, QStringLiteral("build"));
	QCOMPARE(profile.host, QStringLiteral("build.example.com"));
	QCOMPARE(profile.target(), QStringLiteral("nabbe@build.example.com:2222"));
}

void ssh_config_test::an_alias_without_a_hostname_is_its_own_hostname() {
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host router.lan\n"
	        "    User admin\n"));

	QCOMPARE(hosts.size(), 1);
	QCOMPARE(hosts.at(0).hostname, QStringLiteral("router.lan"));
	QCOMPARE(int(hosts.at(0).port), 22);
}

void ssh_config_test::the_first_value_wins_not_the_last() {
	/*
	 * OpenSSH takes the FIRST value obtained for each keyword, which is
	 * backwards from every config format this one resembles. A parser
	 * that takes the last produces entries that look right and connect
	 * somewhere else -- here, as the wrong user on the wrong port.
	 */
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host prod\n"
	        "    User first\n"
	        "    Port 2200\n"
	        "    User second\n"
	        "    Port 2201\n"));

	QCOMPARE(hosts.at(0).user, QStringLiteral("first"));
	QCOMPARE(int(hosts.at(0).port), 2200);
}

void ssh_config_test::wildcard_blocks_configure_hosts_without_becoming_one() {
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host *\n"
	        "    User shared\n"
	        "\n"
	        "Host web\n"
	        "    HostName web.internal\n"
	        "\n"
	        "Host db\n"
	        "    HostName db.internal\n"
	        "    User dba\n"));

	/* `Host *` is not a machine, so it is not offered as one. */
	QCOMPARE(aliases_of(hosts), QStringList({ QStringLiteral("web"), QStringLiteral("db") }));

	/*
	 * But it still configures them -- and since it is FIRST in the file,
	 * its User wins even over the one db states for itself. That is
	 * genuinely what `ssh db` does, however surprising it looks.
	 */
	QCOMPARE(find(hosts, QStringLiteral("web")).user, QStringLiteral("shared"));
	QCOMPARE(find(hosts, QStringLiteral("db")).user, QStringLiteral("shared"));

	/* The same file with the catch-all last: now db speaks for itself. */
	const QList<bssh_ssh_config_host_t> reversed = bssh_ssh_config::parse(QStringLiteral(
	        "Host db\n"
	        "    HostName db.internal\n"
	        "    User dba\n"
	        "\n"
	        "Host *\n"
	        "    User shared\n"));
	QCOMPARE(find(reversed, QStringLiteral("db")).user, QStringLiteral("dba"));
}

void ssh_config_test::negated_patterns_veto_a_block() {
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host !bastion *.internal\n"
	        "    User estate\n"
	        "\n"
	        "Host bastion.internal\n"
	        "    HostName bastion.internal\n"
	        "\n"
	        "Host app.internal\n"));

	/*
	 * A negated pattern that matches vetoes the block however many
	 * positive ones also match, so the bastion does not pick up the
	 * estate-wide user. Note it is `!bastion`, which does NOT match
	 * "bastion.internal" -- patterns are not prefixes.
	 */
	QCOMPARE(find(hosts, QStringLiteral("app.internal")).user, QStringLiteral("estate"));
	QCOMPARE(find(hosts, QStringLiteral("bastion.internal")).user, QStringLiteral("estate"));

	const QList<bssh_ssh_config_host_t> vetoed = bssh_ssh_config::parse(QStringLiteral(
	        "Host !bastion.internal *.internal\n"
	        "    User estate\n"
	        "\n"
	        "Host bastion.internal\n"
	        "Host app.internal\n"));
	QCOMPARE(find(vetoed, QStringLiteral("app.internal")).user, QStringLiteral("estate"));
	QVERIFY2(find(vetoed, QStringLiteral("bastion.internal")).user.isEmpty(),
	          "a matching negated pattern must veto the whole block");
}

void ssh_config_test::match_blocks_are_skipped_rather_than_guessed_at() {
	/*
	 * Match depends on the command line, the local user, and possibly the
	 * exit status of a program, none of which is knowable while reading
	 * the file. Its contents must not be attributed to the Host block
	 * above it -- that would hand the user a profile the config never
	 * described, which is worse than an incomplete one.
	 */
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host gateway\n"
	        "    HostName gw.example.com\n"
	        "\n"
	        "Match exec \"test -n $INSIDE\"\n"
	        "    User insider\n"
	        "    Port 2022\n"));

	QCOMPARE(hosts.size(), 1);
	QVERIFY2(find(hosts, QStringLiteral("gateway")).user.isEmpty(),
	          "a Match block's settings must not leak onto the previous Host");
	QCOMPARE(int(find(hosts, QStringLiteral("gateway")).port), 22);

	/* And a Host after a Match resumes normally. */
	const QList<bssh_ssh_config_host_t> after = bssh_ssh_config::parse(QStringLiteral(
	        "Match user root\n"
	        "    Port 2022\n"
	        "\n"
	        "Host normal\n"
	        "    User someone\n"));
	QCOMPARE(find(after, QStringLiteral("normal")).user, QStringLiteral("someone"));
	QCOMPARE(int(find(after, QStringLiteral("normal")).port), 22);
}

void ssh_config_test::separators_and_comments_and_quotes() {
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "# a comment line\n"
	        "Host=equals\n"
	        "    HostName = spaced.example.com   # trailing comment\n"
	        "    User \"quoted name\"\n"
	        "\n"
	        "   Host indented\n"
	        "\tHostName\ttabbed.example.com\n"));

	QCOMPARE(find(hosts, QStringLiteral("equals")).hostname,
	          QStringLiteral("spaced.example.com"));
	QCOMPARE(find(hosts, QStringLiteral("equals")).user, QStringLiteral("quoted name"));
	QCOMPARE(find(hosts, QStringLiteral("indented")).hostname,
	          QStringLiteral("tabbed.example.com"));
}

void ssh_config_test::unsupported_directives_are_reported_not_dropped() {
	/*
	 * IdentityFile is honoured now -- it becomes the profile's key and
	 * reaches libssh as SSH_OPTIONS_ADD_IDENTITY -- so it is no longer
	 * "unsupported". ProxyJump still has nothing to be applied to, and a
	 * host that silently loses its jump host looks like an app that
	 * cannot reach the machine, so it is parsed to be reported.
	 */
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host deep\n"
	        "    HostName deep.internal\n"
	        "    IdentityFile ~/.ssh/id_work\n"
	        "    ProxyJump bastion.example.com\n"
	        "\n"
	        "Host plain\n"
	        "    HostName plain.example.com\n"));

	QCOMPARE(find(hosts, QStringLiteral("deep")).identity_file,
	          QStringLiteral("~/.ssh/id_work"));
	QCOMPARE(find(hosts, QStringLiteral("deep")).proxy_jump,
	          QStringLiteral("bastion.example.com"));
	/* The key rides along into the profile; the jump host cannot. */
	QCOMPARE(find(hosts, QStringLiteral("deep")).to_profile().identity_file,
	          QStringLiteral("~/.ssh/id_work"));
	QVERIFY(find(hosts, QStringLiteral("deep")).has_unsupported());
	QVERIFY(!find(hosts, QStringLiteral("plain")).has_unsupported());

	const QList<bssh_ssh_config_host_t> keyed = bssh_ssh_config::parse(QStringLiteral(
	        "Host keyed\n  HostName keyed.example.com\n  IdentityFile ~/.ssh/id_only\n"));
	QVERIFY2(!find(keyed, QStringLiteral("keyed")).has_unsupported(),
	          "a named key is imported, so it must not be reported as lost");
}

void ssh_config_test::a_jump_host_spec_is_taken_apart() {
	const QString me = QStringLiteral("local");

	const bssh_jump_tunnel::hop_t bare =
	        bssh_jump_tunnel::parse(QStringLiteral("bastion.example.com"), me);
	QVERIFY(bare.valid);
	QCOMPARE(bare.host, QStringLiteral("bastion.example.com"));
	QCOMPARE(bare.port, quint16(22));
	/*
	 * The user we would have used anyway. OpenSSH's ProxyJump inherits
	 * the connection's user when the spec does not name one, and a
	 * bastion dialled as somebody else is a login failure with nothing on
	 * screen to explain it.
	 */
	QCOMPARE(bare.user, me);

	const bssh_jump_tunnel::hop_t full =
	        bssh_jump_tunnel::parse(QStringLiteral("jane@bastion.example.com:2222"), me);
	QVERIFY(full.valid);
	QCOMPARE(full.host, QStringLiteral("bastion.example.com"));
	QCOMPARE(full.user, QStringLiteral("jane"));
	QCOMPARE(full.port, quint16(2222));

	/* Whitespace around a pasted value is not part of the hostname. */
	const bssh_jump_tunnel::hop_t padded =
	        bssh_jump_tunnel::parse(QStringLiteral("  bastion.example.com  "), me);
	QVERIFY(padded.valid);
	QCOMPARE(padded.host, QStringLiteral("bastion.example.com"));

	/*
	 * A colon that is not a port. An IPv6 literal is written [::1] in
	 * this position, and splitting it on its last colon would dial
	 * something that is not a machine.
	 */
	const bssh_jump_tunnel::hop_t v6 =
	        bssh_jump_tunnel::parse(QStringLiteral("[2001:db8::1]"), me);
	QVERIFY(v6.valid);
	QCOMPARE(v6.host, QStringLiteral("[2001:db8::1]"));
	QCOMPARE(v6.port, quint16(22));

	/* Nothing typed is a direct connection, not a broken jump host. */
	QVERIFY(!bssh_jump_tunnel::parse(QString(), me).valid);
	QVERIFY(!bssh_jump_tunnel::parse(QStringLiteral("   "), me).valid);
}

void ssh_config_test::a_chain_of_jump_hosts_is_refused_not_truncated() {
	/*
	 * Taking only the first hop of "a,b" would connect through a machine
	 * the user named as a STEP and treat it as the destination's gateway.
	 * That is a different route than the one they wrote, and it either
	 * fails confusingly or -- worse -- works, by a path nobody chose.
	 */
	const bssh_jump_tunnel::hop_t chain = bssh_jump_tunnel::parse(
	        QStringLiteral("one.example.com,two.example.com"), QStringLiteral("local"));
	QVERIFY2(!chain.valid, "a chain must be refused rather than half-honoured");
	QVERIFY2(chain.host != QStringLiteral("one.example.com"),
	          "the first hop must not be mistaken for the whole route");
}

void ssh_config_test::junk_does_not_produce_junk_hosts() {
	QVERIFY(bssh_ssh_config::parse(QString()).isEmpty());
	QVERIFY(bssh_ssh_config::parse(QStringLiteral("\n\n   \n")).isEmpty());

	/* Directives before any Host block belong to nothing and are dropped. */
	QVERIFY(bssh_ssh_config::parse(QStringLiteral("User orphan\nPort 2222\n")).isEmpty());

	/* A file of nothing but patterns describes no machine anyone can reach. */
	QVERIFY(bssh_ssh_config::parse(QStringLiteral("Host *\n  User x\n")).isEmpty());

	/*
	 * An unusable port falls back to 22 rather than to 0: a profile that
	 * cannot connect at all is a worse answer than the default the user
	 * would have got from a config without the line.
	 */
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::parse(QStringLiteral(
	        "Host odd\n  Port not-a-number\n"));
	QCOMPARE(int(hosts.at(0).port), 22);

	const QList<bssh_ssh_config_host_t> huge = bssh_ssh_config::parse(QStringLiteral(
	        "Host odd\n  Port 99999\n"));
	QCOMPARE(int(huge.at(0).port), 22);
}

void ssh_config_test::only_the_ticked_hosts_are_imported() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_ssh_config_import importer(&store);
	importer.set_hosts(bssh_ssh_config::parse(QStringLiteral(
	        "Host keep\n  HostName keep.example.com\n"
	        "Host drop\n  HostName drop.example.com\n"
	        "Host also-keep\n  HostName also.example.com\n")));

	/*
	 * The list is the consent. A real ~/.ssh/config accumulates machines
	 * that no longer exist and aliases for things that are not shells, so
	 * importing the unticked ones would be the rude version of this.
	 */
	QListWidget *list = importer.findChild<QListWidget *>();
	QVERIFY(list);
	QCOMPARE(list->count(), 3);
	list->item(1)->setCheckState(Qt::Unchecked);

	QCOMPARE(importer.import_selected(), 2);

	QStringList names;
	for (const bssh_profile_t &profile : store.profiles()) names.append(profile.name);
	names.sort();
	QCOMPARE(names, QStringList({ QStringLiteral("also-keep"), QStringLiteral("keep") }));
}

void ssh_config_test::an_import_never_overwrites_an_existing_host() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t mine;
	mine.name = QStringLiteral("build");
	mine.host = QStringLiteral("build.mine.example.com");
	mine.user = QStringLiteral("me");
	QVERIFY(store.save(mine) != 0);

	bssh_ssh_config_import importer(&store);
	importer.set_hosts(bssh_ssh_config::parse(QStringLiteral(
	        "Host build\n  HostName build.theirs.example.com\n  User them\n")));
	QCOMPARE(importer.import_selected(), 1);

	/*
	 * The config file and the store are independent lists once a host has
	 * been imported, so a second import must not silently rewrite one the
	 * user has since edited here -- it lands beside it instead.
	 */
	QCOMPARE(store.profiles().size(), 2);

	bool found_original = false;
	bool found_imported = false;
	for (const bssh_profile_t &profile : store.profiles()) {
		if (profile.name == QStringLiteral("build")) {
			found_original = true;
			QCOMPARE(profile.host, QStringLiteral("build.mine.example.com"));
		}
		if (profile.name == QStringLiteral("build (imported)")) {
			found_imported = true;
			QCOMPARE(profile.host, QStringLiteral("build.theirs.example.com"));
		}
	}
	QVERIFY2(found_original, "the host the user already had must survive untouched");
	QVERIFY2(found_imported, "the imported one must arrive under a distinct name");
}
