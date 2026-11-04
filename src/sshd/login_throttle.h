#ifndef BSSH_LOGIN_THROTTLE_H
#define BSSH_LOGIN_THROTTLE_H

#include <QHash>
#include <QString>

#include <algorithm>
#include <chrono>
#include <mutex>

/*
 * Wrong passwords, counted per address across connections (sec 19).
 *
 * A connection's own limits say nothing about the next connection: three
 * tries and a second's pause each, four at once, is twelve guesses a
 * second for as long as the server is up -- and this one is meant to be up
 * all the time, so a six-digit PIN lasts a day. Six wrong are free, which
 * is a person mistyping on a phone twice over; each one after that doubles
 * the wait, to an hour, about twenty-four guesses a day from then on.
 *
 * A key is not asked about: it cannot be guessed, so somebody hammering
 * from the owner's own network never locks the owner's key out. A right
 * password during a wait is refused like a wrong one, or the wait would
 * answer whether a guess was right. Nothing here reaches the disk: a
 * restart forgives, which needs the phone in hand.
 */
class bssh_login_throttle {
public:
	using clock = std::chrono::steady_clock;

	static const int free_failures = 6;
	static const int max_addresses = 1024;

	/* Whether a password from this address may be tried now. */
	bool allowed(const QString &address, clock::time_point now) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		const auto entry = m_addresses.constFind(address);
		return entry == m_addresses.constEnd() || now >= entry->wait_until;
	}

	void failed(const QString &address, clock::time_point now) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		forget_old(now);

		entry_t &entry = m_addresses[address];
		entry.last = now;
		++entry.failures;

		const int over = entry.failures - free_failures;
		if (over <= 0) return;
		const std::chrono::seconds wait(1LL << std::min(over, 12));
		entry.wait_until = now + std::min<std::chrono::seconds>(wait, std::chrono::hours(1));
	}

	void succeeded(const QString &address) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_addresses.remove(address);
	}

	int addresses() {
		const std::lock_guard<std::mutex> lock(m_mutex);
		return int(m_addresses.size());
	}

private:
	struct entry_t {
		int failures = 0;
		clock::time_point last;
		clock::time_point wait_until;
	};

	/*
	 * A day of quiet forgives, and the table cannot grow without bound
	 * however many addresses knock: past the limit the longest quiet one
	 * goes, since that is the one least likely to be coming back.
	 */
	void forget_old(clock::time_point now) {
		for (auto entry = m_addresses.begin(); entry != m_addresses.end();) {
			if (now - entry->last > std::chrono::hours(24)) entry = m_addresses.erase(entry);
			else ++entry;
		}
		if (m_addresses.size() < max_addresses) return;

		auto quietest = m_addresses.begin();
		for (auto entry = m_addresses.begin(); entry != m_addresses.end(); ++entry) {
			if (entry->last < quietest->last) quietest = entry;
		}
		m_addresses.erase(quietest);
	}

	std::mutex m_mutex;
	QHash<QString, entry_t> m_addresses;
};

#endif
