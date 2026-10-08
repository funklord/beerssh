#include "keyboard/sk_modifiers.h"

sk_modifiers::sk_modifiers(QObject *parent) : QObject(parent) {}

void sk_modifiers::set_armed(quint8 modifiers) {
	if (m_armed == modifiers) return;
	m_armed = modifiers;
	emit armed_changed(m_armed);
}

void sk_modifiers::set_locked(quint8 modifiers) {
	if (m_locked == modifiers) return;
	m_locked = modifiers;
	emit locked_changed(m_locked);
}

sk_modifiers::state_t sk_modifiers::state(quint8 modifier) const {
	if (m_locked & modifier) return LOCKED;
	if (m_held & modifier) return HELD;
	if (m_armed & modifier) return ONCE;
	return OFF;
}

void sk_modifiers::cycle(quint8 modifier) {
	if (m_locked & modifier) {
		set_locked(quint8(m_locked & ~modifier));
	} else if (m_armed & modifier) {
		set_armed(quint8(m_armed & ~modifier));
		set_locked(quint8(m_locked | modifier));
	} else {
		set_armed(quint8(m_armed | modifier));
	}
}

void sk_modifiers::hold(quint8 modifier) {
	m_held_used = quint8(m_held_used & ~modifier);
	if (m_held & modifier) return;
	m_held = quint8(m_held | modifier);
	emit held_changed(m_held);
}

bool sk_modifiers::release(quint8 modifier) {
	if (!(m_held & modifier)) return false;
	const bool tapped = !(m_held_used & modifier);
	m_held = quint8(m_held & ~modifier);
	m_held_used = quint8(m_held_used & ~modifier);
	emit held_changed(m_held);
	return tapped;
}

void sk_modifiers::spend() {
	m_held_used = quint8(m_held_used | m_held);
	set_armed(SK_MOD_NONE);
}
