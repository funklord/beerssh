#include <QRegularExpression>
#include <QtTest>

#include "model/command_line.h"
#include "model/profile.h"
#include "command_line_test.h"

namespace {

/* argv[0] is skipped by the parser, so every case has to carry one. */
bssh_command_line_t parse(const QStringList &arguments) {
	return bssh_parse_command_line(QStringList(QStringLiteral("beerssh")) + arguments);
}

} /* namespace */

void command_line_test::a_bare_destination_is_the_target() {
	const bssh_command_line_t line = parse({ QStringLiteral("user@host:2222") });

	QVERIFY(line.error.isEmpty());
	QCOMPARE(line.target, QStringLiteral("user@host:2222"));
	QVERIFY(line.opens_a_session());
}

void command_line_test::a_destination_becomes_a_profile() {
	/*
	 * The two directions of one mapping. bssh_profile_t::target() writes
	 * what from_target() reads, and they live beside each other because
	 * they used to be a hundred lines apart -- the window parsed a
	 * destination and the profile printed one, and nothing checked that
	 * the pair round-tripped.
	 */
	const bssh_profile_t profile =
	        bssh_profile_t::from_target(QStringLiteral("nabbe@example.invalid:2222"));

	QCOMPARE(profile.user, QStringLiteral("nabbe"));
	QCOMPARE(profile.host, QStringLiteral("example.invalid"));
	QCOMPARE(profile.port, quint16(2222));
	QCOMPARE(profile.target(), QStringLiteral("nabbe@example.invalid:2222"));

	/* A bare host keeps the default port, which target() then omits. */
	const bssh_profile_t plain = bssh_profile_t::from_target(QStringLiteral("example.invalid"));
	QCOMPARE(plain.host, QStringLiteral("example.invalid"));
	QCOMPARE(plain.port, quint16(22));
	QVERIFY(!plain.target().contains(QLatin1Char(':')));
}

void command_line_test::execute_takes_the_rest_of_the_line() {
	/*
	 * The whole reason -e is split off before the option parser sees
	 * anything: `-f` here belongs to tail, and a parser that read it as
	 * ours would refuse the command line xterm has accepted since 1984.
	 */
	const bssh_command_line_t line = parse({
		QStringLiteral("-T"), QStringLiteral("Logs"),
		QStringLiteral("-e"), QStringLiteral("tail"),
		QStringLiteral("-f"), QStringLiteral("/var/log/app.log"),
	});

	QVERIFY2(line.error.isEmpty(), qPrintable(line.error));
	QCOMPARE(line.title, QStringLiteral("Logs"));
	QCOMPARE(line.command, QStringList({
		QStringLiteral("tail"), QStringLiteral("-f"),
		QStringLiteral("/var/log/app.log"),
	}));

	/* And --hold after -e is the COMMAND's argument, not ours. */
	const bssh_command_line_t after = parse({
		QStringLiteral("-e"), QStringLiteral("ls"), QStringLiteral("--hold"),
	});
	QVERIFY(!after.hold);
	QCOMPARE(after.command, QStringList({ QStringLiteral("ls"), QStringLiteral("--hold") }));
}

void command_line_test::execute_with_an_equals_sign_is_one_word() {
	const bssh_command_line_t line = parse({ QStringLiteral("--execute=htop") });

	QVERIFY(line.error.isEmpty());
	QCOMPARE(line.command, QStringList(QStringLiteral("htop")));
}

void command_line_test::execute_with_nothing_after_it_is_refused() {
	const bssh_command_line_t line = parse({ QStringLiteral("-e") });

	QVERIFY(!line.error.isEmpty());
	QVERIFY(line.command.isEmpty());
}

void command_line_test::geometry_is_counted_in_cells() {
	const bssh_command_line_t line = parse({
		QStringLiteral("--geometry"), QStringLiteral("120x40+10-20"),
	});

	QVERIFY2(line.error.isEmpty(), qPrintable(line.error));
	QCOMPARE(line.cols, 120);
	QCOMPARE(line.rows, 40);
	QVERIFY(line.has_position);
	QCOMPARE(line.x, 10);
	QCOMPARE(line.y, -20);

	/* The position is optional, and its absence is not a zero position. */
	const bssh_command_line_t plain = parse({
		QStringLiteral("--geometry"), QStringLiteral("80x24"),
	});
	QVERIFY(!plain.has_position);
	QCOMPARE(plain.cols, 80);
}

void command_line_test::geometry_that_is_not_a_grid_is_refused() {
	QVERIFY(!parse({ QStringLiteral("--geometry"), QStringLiteral("80") }).error.isEmpty());
	QVERIFY(!parse({ QStringLiteral("--geometry"), QStringLiteral("800x600pixels") })
	                 .error.isEmpty());
	QVERIFY(!parse({ QStringLiteral("--geometry"), QStringLiteral("0x24") }).error.isEmpty());

	/*
	 * And a number nobody meant. A window sized to a hundred thousand
	 * columns is off every screen it could be put on and cannot be
	 * dragged back, so it is refused rather than obeyed.
	 */
	QVERIFY(!parse({ QStringLiteral("--geometry"), QStringLiteral("100000x24") })
	                 .error.isEmpty());
}

void command_line_test::an_ini_path_is_a_document_and_not_a_host() {
	const bssh_command_line_t line = parse({ QStringLiteral("/tmp/servers.ini") });

	QVERIFY(line.error.isEmpty());
	QCOMPARE(line.document, QStringLiteral("/tmp/servers.ini"));
	QVERIFY2(line.target.isEmpty(), "a preset path must not be dialled as a hostname");

	/*
	 * Decided on the suffix ALONE, and that is the fix rather than an
	 * oversight: the rule this replaced also required the file to exist,
	 * so a mistyped preset path fell through to being treated as a
	 * destination and the app went off to resolve "presets.ini". Whether
	 * the file is there is the importer's question, and it has a message
	 * for the answer.
	 */
	QCOMPARE(parse({ QStringLiteral("/nowhere/missing.ini") }).document,
	          QStringLiteral("/nowhere/missing.ini"));

	/* A file manager may hand over a URL rather than a path. */
	QCOMPARE(parse({ QStringLiteral("file:///tmp/servers.ini") }).document,
	          QStringLiteral("/tmp/servers.ini"));

	/* Both at once is the ordinary case for a handler: import and connect. */
	const bssh_command_line_t both = parse({
		QStringLiteral("/tmp/servers.ini"), QStringLiteral("host"),
	});
	QCOMPARE(both.document, QStringLiteral("/tmp/servers.ini"));
	QCOMPARE(both.target, QStringLiteral("host"));
}

void command_line_test::two_destinations_are_refused() {
	const bssh_command_line_t line = parse({ QStringLiteral("one"), QStringLiteral("two") });

	QVERIFY2(!line.error.isEmpty(), "two hostnames must not silently pick one");
	QVERIFY(line.error.contains(QStringLiteral("one")));
	QVERIFY(line.error.contains(QStringLiteral("two")));
}

void command_line_test::load_and_a_destination_are_refused() {
	const bssh_command_line_t line = parse({
		QStringLiteral("--load"), QStringLiteral("prod"), QStringLiteral("host"),
	});

	QVERIFY(!line.error.isEmpty());
}

void command_line_test::workdir_over_ssh_is_refused() {
	/*
	 * SSH has no way to ask a server to start somewhere, so honouring
	 * this would mean ignoring it. Refused with a reason instead: an
	 * option that silently does nothing is how people stop trusting a
	 * program's arguments.
	 */
	const bssh_command_line_t over_ssh = parse({
		QStringLiteral("--workdir"), QStringLiteral("/srv"), QStringLiteral("host"),
	});
	QVERIFY(!over_ssh.error.isEmpty());

	const bssh_command_line_t over_saved = parse({
		QStringLiteral("--workdir"), QStringLiteral("/srv"),
		QStringLiteral("--load"), QStringLiteral("prod"),
	});
	QVERIFY(!over_saved.error.isEmpty());

	/* Locally it is exactly what it says. */
	const bssh_command_line_t local = parse({
		QStringLiteral("--workdir"), QStringLiteral("/srv"),
	});
	QVERIFY2(local.error.isEmpty(), qPrintable(local.error));
	QCOMPARE(local.working_directory, QStringLiteral("/srv"));
	QVERIFY2(local.opens_a_session(),
	          "--workdir on its own still names the session it applies to");
}

void command_line_test::hold_and_title_and_self_test_are_read() {
	const bssh_command_line_t line = parse({
		QStringLiteral("--hold"),
		QStringLiteral("--title"), QStringLiteral("Build box"),
		QStringLiteral("--self-test"),
	});

	QVERIFY(line.error.isEmpty());
	QVERIFY(line.hold);
	QVERIFY(line.self_test);
	QCOMPARE(line.title, QStringLiteral("Build box"));
}

void command_line_test::an_unknown_option_is_refused() {
	const bssh_command_line_t line = parse({ QStringLiteral("--colour=green") });

	QVERIFY2(!line.error.isEmpty(), "an option nobody implements must not be ignored");
}

void command_line_test::help_and_version_ask_for_nothing_else() {
	const bssh_command_line_t help = parse({ QStringLiteral("--help") });
	QVERIFY(help.help);
	QVERIFY(help.exits_immediately());

	QVERIFY(parse({ QStringLiteral("-h") }).help);
	QVERIFY(parse({ QStringLiteral("--version") }).version);
	QVERIFY(parse({ QStringLiteral("-v") }).version);
}

void command_line_test::every_option_is_documented_and_every_documented_option_parses() {
	/*
	 * Both directions, because either alone is satisfiable by doing
	 * nothing. An option missing from the help is one nobody can find; a
	 * line in the help that the parser rejects is a lie the program tells
	 * about itself. sec 6.1 records the same failure for the stolen
	 * chords, where the only way to learn which chords never reached the
	 * shell was to press one.
	 */
	const QString usage = bssh_command_line_usage();

	for (const bssh_option_t &option : bssh_command_line_options()) {
		const QString spelled = QStringLiteral("--") + QString::fromLatin1(option.name);

		QVERIFY2(usage.contains(spelled),
		          qPrintable(QStringLiteral("%1 is accepted and undocumented").arg(spelled)));

		if (*option.short_name) {
			QVERIFY2(usage.contains(QLatin1Char('-') + QString::fromLatin1(option.short_name)),
			          qPrintable(QStringLiteral("%1 has an undocumented short form")
			                             .arg(spelled)));
		}

		/*
		 * And it really parses. A flag is given nothing, an option with
		 * a value is given one that its own validator accepts -- the
		 * point is that the parser knows the NAME, which an unknown
		 * option's error would say it did not.
		 */
		QStringList arguments(spelled);
		if (*option.value_name) {
			arguments.append(QString::fromLatin1(option.name)
			                                 == QStringLiteral("geometry")
			                         ? QStringLiteral("80x24")
			                         : QStringLiteral("x"));
		}

		const bssh_command_line_t line = parse(arguments);
		QVERIFY2(!line.error.contains(QStringLiteral("Unknown option")),
		          qPrintable(QStringLiteral("%1 is documented and not accepted: %2")
		                             .arg(spelled, line.error)));
	}
}

void command_line_test::the_manual_page_describes_the_options_the_parser_takes() {
	/*
	 * sec 8.6's manual page, held to the table it is generated from --
	 * which is the whole reason it is generated. A page kept as a file
	 * documents a flag that no longer exists and omits the one you
	 * wanted, and neither is an error anybody sees.
	 */
	const QString page = bssh_command_line_man_page(QStringLiteral("9.9.9"));

	QVERIFY2(page.startsWith(QStringLiteral(".TH BEERSSH 1 ")),
	          "a manual page starts with its .TH header or man will not index it");
	QVERIFY2(page.contains(QStringLiteral("beerssh 9.9.9")),
	          "the version passed in must reach the header");

	for (const bssh_option_t &option : bssh_command_line_options()) {
		/*
		 * Spelled as roff spells it: every hyphen escaped, or the
		 * reader is shown a hyphenation point instead of the minus
		 * they are meant to type.
		 */
		const QString spelled = QStringLiteral("\\fB\\-\\-%1\\fR")
		                                .arg(QString::fromLatin1(option.name));

		QVERIFY2(page.contains(spelled),
		          qPrintable(QStringLiteral("--%1 is accepted and not in the manual page")
		                             .arg(QString::fromLatin1(option.name))));
	}

	/*
	 * And the other direction, which is the half that catches an option
	 * REMOVED from the parser and left in the page. Every .TP entry
	 * naming a long option has to be one the table still carries.
	 */
	static const QRegularExpression entry(
	        QStringLiteral("\\\\fB\\\\-\\\\-([a-z-]+)\\\\fR"));

	QRegularExpressionMatchIterator found = entry.globalMatch(page);
	int seen = 0;

	while (found.hasNext()) {
		const QString named = found.next().captured(1);
		bool known = false;

		for (const bssh_option_t &option : bssh_command_line_options()) {
			if (named == QString::fromLatin1(option.name)) known = true;
		}

		QVERIFY2(known, qPrintable(QStringLiteral("the manual page documents --%1, "
		                                           "which the parser does not take")
		                                   .arg(named)));
		++seen;
	}

	QVERIFY2(seen >= bssh_command_line_options().size(),
	          "the page must name every option, not a subset that happens to match");
}
