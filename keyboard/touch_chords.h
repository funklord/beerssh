#ifndef SK_TOUCH_CHORDS_H
#define SK_TOUCH_CHORDS_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>

class QAbstractButton;
class QTouchEvent;
class QWidget;

/*
 * Two thumbs on an on-screen keyboard (project.md sec 6.2): a finger held
 * on Ctrl while another presses C sends Ctrl-C, as on a physical keyboard.
 *
 * Qt cannot do this with plain buttons. It turns only ONE touch into
 * mouse events, so a second finger landing on another key while the
 * first is down presses nothing at all -- which is why the boards were
 * sticky-only. This takes a board's touches and gives every finger its
 * own button.
 *
 * It takes them ONLY once a chord is under way: a touch beginning on a
 * modifier, or any touch while a modifier is held. Every other touch is
 * left alone, so a single tap reaches its button the way it always has,
 * and the key row's drag-to-scroll -- a QScroller fed by those same
 * synthesized mouse events -- keeps working.
 *
 * A finger on a key is handed to its button as mouse events, so the
 * button does what it already does: draws pressed, repeats when held,
 * sends on release, and sends nothing when the finger slides off. A
 * finger on a modifier is reported instead, and the board turns that
 * into a hold on the router; a modifier button is one whose
 * "sk_hold_modifier" property is set to the modifier it stands for.
 */
class sk_touch_chords : public QObject {
	Q_OBJECT

public:
	explicit sk_touch_chords(QWidget *board);

	/*
	 * A further board sharing these fingers: the split keyboard's halves
	 * (sec 6.2), so Ctrl held on one half carries a key on the other.
	 */
	void watch(QWidget *board);

	/*
	 * Lets go of every finger without sending anything: keys are released
	 * off their buttons, modifiers reported as cancelled. For a board
	 * changing session or rebuilding under a finger.
	 */
	void cancel();

signals:
	void modifier_held(quint8 modifier);

	/*
	 * `cancelled` is true when the touch did not end with a lift -- the
	 * system took it, or cancel() was called -- so the board releases the
	 * hold without reading it as a tap.
	 */
	void modifier_released(quint8 modifier, bool cancelled);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	struct finger_t {
		QPointer<QAbstractButton> button;
		quint8 modifier;
	};

	bool touch(QTouchEvent *event);
	QAbstractButton *button_at(const QPointF &global) const;
	quint8 modifier_of(const QAbstractButton *button) const;
	void release(finger_t &finger, const QPointF &global, bool cancelled);

	QList<QPointer<QWidget>> m_boards;
	QHash<int, finger_t> m_fingers;
};

#endif /* SK_TOUCH_CHORDS_H */
