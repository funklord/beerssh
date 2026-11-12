#ifndef BSSH_MEDIA_PLAYER_H
#define BSSH_MEDIA_PLAYER_H

#include <QString>

#include <functional>
#include <memory>

class QWidget;
class bssh_media_bytes;

/*
 * Playing a file from the file panel (project.md sec 6.4, *Playing a
 * file*): one thing played, from wherever its bytes are.
 */
struct bssh_media_t {
	QString name;                             /* the file's name; its kind is read from it */
	QString path;                             /* a local path a player may open, when there is one */
	std::shared_ptr<bssh_media_bytes> bytes;  /* the bytes as they arrive, for a player reading them */
};

/* Audio or video, by the name: what Play is offered for. */
bool bssh_is_playable(const QString &name);

/*
 * The player inside BeerSSH, where the build has one: plays `media` in
 * `area` and says true, or false and the panel opens the file in the
 * desktop's player instead. Empty in the default build; the media build
 * (`make BSSH_MEDIA=1`) sets it to snurra's once snurra offers its
 * embedding facade (snurra/embed.h), and tests set their own.
 */
using bssh_in_app_player_t = std::function<bool(const bssh_media_t &media, QWidget *area)>;
bssh_in_app_player_t &bssh_in_app_player();

#endif
