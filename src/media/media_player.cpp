#include "media/media_player.h"

#include <QMimeDatabase>
#include <QMimeType>

#include "media/media_bytes.h"

bool bssh_is_playable(const QString &name) {
	const QMimeType type = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension);
	const QString kind = type.name();
	return kind.startsWith(QLatin1String("audio/")) || kind.startsWith(QLatin1String("video/"));
}

/*
 * The media build's player: snurra's, through the one header it offers a
 * host application (snurra/embed.h, proposed to snurra 2026-11-12 and
 * recorded in project.md sec 6.4). Until a pinned snurra has that header
 * the media build compiles this out and plays in the desktop's player,
 * so BeerSSH takes snurra's player up the day the pin moves past it,
 * with nothing here to change. Nothing below the facade is used: snurra's
 * engine, decks and tiles are its own to rework.
 */
#if defined(BSSH_MEDIA) && __has_include("snurra/embed.h")
#include "snurra/embed.h"

#include <QLayout>
#include <QWidget>

namespace {

/* BeerSSH's arriving file as snurra reads bytes: the same four calls. */
class arriving_bytes : public snurra_bytes {
public:
	explicit arriving_bytes(std::shared_ptr<bssh_media_bytes> bytes) : m_bytes(std::move(bytes)) {}
	int64_t size() const override { return m_bytes->size(); }
	int64_t read(int64_t offset, uint8_t *out, int64_t n) override { return m_bytes->read(offset, out, n); }
	bool has(int64_t offset, int64_t n) const override { return m_bytes->has(offset, n); }
	void want(int64_t offset) override { m_bytes->want(offset); }
	void wait(int ms) override { m_bytes->wait(ms); }

private:
	std::shared_ptr<bssh_media_bytes> m_bytes;
};

bool play_in_snurra(const bssh_media_t &media, QWidget *area) {
	if (!area || !snurra_player::can_play(media.name)) return false;
	/* One player per area, its tiles a train: a second Play joins it. */
	snurra_player *player = area->findChild<snurra_player *>(QStringLiteral("media-player"));
	if (!player) {
		player = new snurra_player(area);
		player->setObjectName(QStringLiteral("media-player"));
		/* Into the area's layout: a child not in it is given no room, and
		 * the first player shown was a sliver (2026-11-12). */
		if (area->layout()) area->layout()->addWidget(player);
		QObject::connect(player, &snurra_player::emptied, player, &QObject::deleteLater);
	}
	snurra_media wanted;
	wanted.name = media.name;
	wanted.path = media.path;
	if (media.bytes) wanted.bytes = std::make_shared<arriving_bytes>(media.bytes);
#if SNURRA_EMBED_VERSION >= 2
	/* The .srt files beside a remote file, fetched first (embed.h 2). */
	wanted.subtitles = media.subtitles;
#endif
	if (!player->open(wanted)) {
		if (player->count() == 0) player->deleteLater();
		return false;
	}
	player->show();
	return true;
}

}

bssh_in_app_player_t &bssh_in_app_player() {
	static bssh_in_app_player_t player = play_in_snurra;
	return player;
}
#else
bssh_in_app_player_t &bssh_in_app_player() {
	static bssh_in_app_player_t player;
	return player;
}
#endif
