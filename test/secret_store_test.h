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
	void the_store_is_replaced_atomically_and_stays_owner_only();
	/*
	 * sec 11 asks the UI to name the backend and say whether it is
	 * hardware-backed, in as many words and without softening. Everything
	 * needed for that existed from the start and nothing showed it: the
	 * one place backend_name() appeared was the unlock prompt, which the
	 * keyring backend never raises -- so the users who most needed to
	 * know were the ones who could not find out.
	 */
	void the_settings_page_names_the_secret_backend();

	/*
	 * And shows all of it. A wrapped label in a form's field column is
	 * clipped rather than grown -- the layout asks it for a height before
	 * it knows the width it will get -- which on a phone cut the backend
	 * name off at "passphrase-encrypted file (no". Losing the end of that
	 * particular label changes what it says.
	 */
	void no_setting_is_cut_off_at_a_phone_width();

	/*
	 * And the prompt that asks for the password in the first place. It
	 * used to put the whole "Password for user@host:port:" in a form
	 * layout's LABEL column, so the dialog's width grew with the length
	 * of the destination -- and on a phone the checkbox and the buttons
	 * ran off the right-hand edge. A password prompt whose OK button is
	 * off-screen is not a cosmetic problem.
	 */
	void the_password_prompt_fits_a_phone();
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
