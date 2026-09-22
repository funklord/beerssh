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

void command_line_test::the_completion_offers_the_options_the_parser_takes() {
	/*
	 * The same both-directions check the manual page gets, and for a
	 * sharper reason. A page that omits a flag is merely unhelpful; a
	 * completion that omits one actively refuses to finish what somebody
	 * is typing, and they conclude the flag does not exist.
	 */
	const QString script = bssh_command_line_completion();

	/*
	 * Asked of whole LINES rather than of the text, because a shell
	 * script's comment marker makes `contains` answer yes about a line
	 * that does nothing. The sabotage that comments the register out
	 * left this green until it was written this way -- the assertion was
	 * true and about the wrong thing, which is how a true condition
	 * passes for the same reason a correct one does.
	 */
	const QStringList lines = script.split(QLatin1Char('\n'));
	bool registers = false;
	bool defines = false;

	for (const QString &line : lines) {
		const QString bare = line.trimmed();
		if (bare.startsWith(QStringLiteral("complete -F _beerssh -o default beerssh")))
			registers = true;
		if (bare.startsWith(QStringLiteral("_beerssh()"))) defines = true;
	}

	QVERIFY2(registers,
	          "the script registers no completion, so sourcing it does nothing");
	QVERIFY2(defines, "the function the complete line names is not defined");

	/*
	 * The word list is the part that has to agree with the table. Pulled
	 * out by itself rather than searched for in the whole script,
	 * because a spelling that appears only in a `case` arm is not one
	 * bash will ever OFFER.
	 */
	static const QRegularExpression words(QStringLiteral("compgen -W \"([^\"]*)\""));

	/*
	 * The list of LITERAL flags, picked out by its contents rather than
	 * by being the first `compgen -W` in the file. It was the first
	 * until `--load` learned to complete session names by asking the
	 * program, whose word list is a command substitution -- and this
	 * assertion then read THAT as the options and reported every flag
	 * missing. A position is not an identity.
	 */
	QString list;
	QRegularExpressionMatchIterator lists = words.globalMatch(script);
	while (lists.hasNext()) {
		const QString candidate = lists.next().captured(1);
		if (candidate.startsWith(QLatin1Char('-'))) list = candidate;
	}
	QVERIFY2(!list.isEmpty(), "the script offers no option list at all");

	const QStringList offered = list.split(QLatin1Char(' '), Qt::SkipEmptyParts);

	for (const bssh_option_t &option : bssh_command_line_options()) {
		const QString name = QStringLiteral("--")
		        + QString::fromLatin1(option.name);
		QVERIFY2(offered.contains(name),
		          qPrintable(QStringLiteral("%1 is accepted and not offered").arg(name)));

		const QString shrt = QString::fromLatin1(option.short_name);
		if (shrt.isEmpty()) continue;

		QVERIFY2(offered.contains(QStringLiteral("-") + shrt),
		          qPrintable(QStringLiteral("-%1 is accepted and not offered").arg(shrt)));
	}

	/*
	 * And the other direction, which catches an option REMOVED from the
	 * parser and left in the script -- the half that makes this a
	 * relationship rather than a spot check.
	 */
	for (const QString &word : offered) {
		bool known = false;

		for (const bssh_option_t &option : bssh_command_line_options()) {
			const QString name = QStringLiteral("--")
			        + QString::fromLatin1(option.name);
			const QString shrt = QString::fromLatin1(option.short_name);

			if (word == name) known = true;
			if (!shrt.isEmpty() && word == QStringLiteral("-") + shrt) known = true;
		}

		QVERIFY2(known, qPrintable(QStringLiteral("the completion offers %1, which "
		                                           "the parser does not take").arg(word)));
	}

	/*
	 * An option that takes a VALUE has to be in a case arm, or bash
	 * offers the option list where a value belongs -- `beerssh -e
	 * --help` as a thing to run. Asked of both spellings, because the
	 * short one is what people type.
	 *
	 * And the arm has to be the RIGHT one, which is the half this check
	 * was missing: every value shape falls back to a silent arm, so
	 * "has an arm" stayed true with the classification deleted. Two
	 * sabotages went MISSED against it -- the one that removes the
	 * whole classification and the one that removes just the session
	 * branch -- because both leave an arm behind, a silent one. The
	 * relationship worth asserting is between the table's value_name
	 * and what the arm DOES.
	 */
	static const QMap<QString, QString> completes_with = {
		{ QStringLiteral("command"), QStringLiteral("compgen -c") },
		{ QStringLiteral("directory"), QStringLiteral("compgen -d") },
		{ QStringLiteral("session"), QStringLiteral("--load=?") },
	};

	for (const bssh_option_t &option : bssh_command_line_options()) {
		const QString value = QString::fromLatin1(option.value_name);
		if (!completes_with.contains(value)) continue;

		const QString name = QStringLiteral("--")
		        + QString::fromLatin1(option.name);

		/*
		 * The arm's BODY, sliced from its label to the `;;` that ends
		 * it, so a match cannot come from a different arm further down.
		 */
		const int at = script.indexOf(name + QStringLiteral(")"));
		const int bar = script.indexOf(name + QStringLiteral("|"));
		const int start = at >= 0 ? at : bar;
		QVERIFY2(start >= 0,
		          qPrintable(QStringLiteral("%1 takes a %2 and has no case arm")
		                             .arg(name, value)));

		const int end = script.indexOf(QStringLiteral(";;"), start);
		QVERIFY2(end > start, qPrintable(QStringLiteral("%1's case arm never ends")
		                                          .arg(name)));

		const QString arm = script.mid(start, end - start);
		QVERIFY2(arm.contains(completes_with.value(value)),
		          qPrintable(QStringLiteral("%1 takes a %2 and its arm does not "
		                                     "use %3, so it completes nothing "
		                                     "where it could")
		                             .arg(name, value, completes_with.value(value))));
	}

	for (const bssh_option_t &option : bssh_command_line_options()) {
		if (!*option.value_name) continue;

		const QString name = QStringLiteral("--")
		        + QString::fromLatin1(option.name);
		QVERIFY2(script.contains(name + QStringLiteral(")"))
		                 || script.contains(name + QStringLiteral("|")),
		          qPrintable(QStringLiteral("%1 takes a value and has no case arm")
		                             .arg(name)));

		const QString shrt = QString::fromLatin1(option.short_name);
		if (shrt.isEmpty()) continue;

		const QString dash = QStringLiteral("-") + shrt;
		QVERIFY2(script.contains(dash + QStringLiteral(")"))
		                 || script.contains(dash + QStringLiteral("|")),
		          qPrintable(QStringLiteral("%1 takes a value and has no case arm")
		                             .arg(dash)));
	}
}

void command_line_test::a_target_with_colons_is_a_host_not_a_port() {
	/*
	 * from_target split on the LAST colon, so every IPv6 literal was
	 * mangled in silence: `2001:db8::1` became host "2001:db8:" on port 1,
	 * and `::1` became host ":" on port 1. Not a failure -- a connection
	 * to somewhere nobody asked for.
	 *
	 * And the port was read with toUShort() and no ok flag, so anything
	 * unparseable or out of range became 0: worse than the 22 it
	 * overwrote, and a failure whose message says nothing about the port.
	 * ssh_config.cpp validated the SAME syntax properly all along, which
	 * is the wrong way round if either path had to be careless -- one
	 * reads a file, the other reads what somebody just typed.
	 */
	const bssh_profile_t plain = bssh_profile_t::from_target(QStringLiteral("2001:db8::1"));
	QCOMPARE(plain.host, QStringLiteral("2001:db8::1"));
	QCOMPARE(plain.port, quint16(22));

	const bssh_profile_t loopback = bssh_profile_t::from_target(QStringLiteral("::1"));
	QCOMPARE(loopback.host, QStringLiteral("::1"));
	QCOMPARE(loopback.port, quint16(22));

	/* Bracketed, which is how OpenSSH writes an address with a port. */
	const bssh_profile_t bracketed =
	        bssh_profile_t::from_target(QStringLiteral("[2001:db8::1]:2222"));
	QCOMPARE(bracketed.host, QStringLiteral("2001:db8::1"));
	QCOMPARE(bracketed.port, quint16(2222));

	/* The ordinary cases still work, which is the control. */
	const bssh_profile_t named = bssh_profile_t::from_target(QStringLiteral("box.example:2200"));
	QCOMPARE(named.host, QStringLiteral("box.example"));
	QCOMPARE(named.port, quint16(2200));

	const bssh_profile_t with_user =
	        bssh_profile_t::from_target(QStringLiteral("ada@box.example"));
	QCOMPARE(with_user.user, QStringLiteral("ada"));
	QCOMPARE(with_user.host, QStringLiteral("box.example"));
	QCOMPARE(with_user.port, quint16(22));

	/* A port that is not one keeps the default rather than becoming 0. */
	QCOMPARE(bssh_profile_t::from_target(QStringLiteral("box.example:abc")).port, quint16(22));
	QCOMPARE(bssh_profile_t::from_target(QStringLiteral("box.example:99999")).port, quint16(22));
	QCOMPARE(bssh_profile_t::from_target(QStringLiteral("box.example:0")).port, quint16(22));
}
