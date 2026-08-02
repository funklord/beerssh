#ifndef BSSH_SECRET_STORE_TEST_H
#define BSSH_SECRET_STORE_TEST_H

#include <QObject>

/*
 * The secret store (project.md sec 11).
 *
 * Worth testing harder than most things here: it is the only component
 * whose failure is silent and permanent. A profile that fails to save is
 * noticed immediately; a credential written where it can be read, or a
 * wrong passphrase that yields garbage a caller then treats as a
 * password, is not.
 */
class secret_store_test : public QObject {
	Q_OBJECT

private slots:
	void stores_and_loads_a_secret();
	void the_wrong_passphrase_is_rejected_not_guessed_at();
	void the_file_does_not_contain_the_secret();
	void tampering_is_detected();
	void a_locked_store_yields_nothing();
	void erasing_removes_it_from_the_file();
	void the_backend_describes_itself_honestly();
	void the_selector_always_returns_a_working_store();
};

#endif /* BSSH_SECRET_STORE_TEST_H */
