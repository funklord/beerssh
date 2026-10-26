#ifndef BSSH_CONFORMANCE_TEST_H
#define BSSH_CONFORMANCE_TEST_H

#include <QObject>

/*
 * Properties that must hold for every feature a terminal speaks, run over a
 * table of them rather than written once per bug (project.md sec 12.3w).
 *
 * The holder, after two rounds of colour defects found by eye: "I can't
 * even begin to guess what problems exist with all the new terminal
 * standards, we need to create better tests." So each test here states one
 * invariant and is fed every feature in its table, and a new sequence is
 * a new row, not a new test:
 *
 *   - what a cell is -- its character, colours and attributes -- survives
 *     the scrollback and a resize that brings it back;
 *   - the window's own pixels, after every read, are what a full repaint
 *     would draw, whatever the read did.
 */
class conformance_test : public QObject {
	Q_OBJECT

private slots:
	void a_cell_keeps_what_it_is_through_scrollback_and_resize_data();
	void a_cell_keeps_what_it_is_through_scrollback_and_resize();

	void the_screen_is_always_what_a_full_repaint_draws_data();
	void the_screen_is_always_what_a_full_repaint_draws();
};

#endif /* BSSH_CONFORMANCE_TEST_H */
