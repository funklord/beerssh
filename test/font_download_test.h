#ifndef BSSH_FONT_DOWNLOAD_TEST_H
#define BSSH_FONT_DOWNLOAD_TEST_H

#include <QObject>

/*
 * A font family fetched onto a phone one file at a time (project.md sec
 * 9.4): the zip reader that makes one file fetchable, the pins it is
 * checked against, and the download driven through a source that is a
 * buffer rather than a network.
 */
class font_download_test : public QObject {
	Q_OBJECT

private slots:
	void a_zip_member_comes_out_whole_stored_or_deflated();
	void a_damaged_or_foreign_zip_is_refused();
	void the_pins_built_in_are_the_whole_set();
	void a_family_downloads_file_by_file_and_is_checked();
	void a_file_that_is_not_the_pinned_one_leaves_nothing();
	void only_a_set_font_that_is_missing_is_offered_for_download();

	/*
	 * The real thing, over the network, against a family's pinned release:
	 * only with BSSH_TEST_LIVE_FONTS=1, since a suite that needs the
	 * internet fails for reasons that are not the code's.
	 */
	void a_real_family_downloads_from_its_release();
};

#endif /* BSSH_FONT_DOWNLOAD_TEST_H */
