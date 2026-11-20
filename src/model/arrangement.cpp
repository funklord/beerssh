#include "arrangement.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

const char FORMAT[] = "beerssh-arrangement";
const int VERSION = 1;

/* A window reaching past any screen, or none, is a geometry not to apply. */
const int MAX_EXTENT = 100000;

}  // namespace

QString bssh_arrangement_t::to_text() const {
	QJsonArray list;
	for (const window_t &window : windows) {
		QJsonObject object;
		object.insert(QStringLiteral("geometry"),
		              QJsonArray{ window.geometry.x(), window.geometry.y(), window.geometry.width(),
		                          window.geometry.height() });
		if (window.maximized) object.insert(QStringLiteral("maximized"), true);
		if (window.full_screen) object.insert(QStringLiteral("full_screen"), true);
		if (window.terminals_panel) object.insert(QStringLiteral("terminals_panel"), true);
		if (!window.shape.isEmpty()) object.insert(QStringLiteral("shape"), window.shape);
		object.insert(QStringLiteral("terminals"), QJsonArray::fromStringList(window.terminals));
		if (!window.current.isEmpty()) object.insert(QStringLiteral("current"), window.current);
		list.append(object);
	}
	QJsonObject root;
	root.insert(QStringLiteral("format"), QLatin1String(FORMAT));
	root.insert(QStringLiteral("version"), VERSION);
	root.insert(QStringLiteral("windows"), list);
	return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bssh_arrangement_t bssh_arrangement_t::from_text(const QString &text, QString *why) {
	bssh_arrangement_t out;
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8(), &error);
	if (!document.isObject()) {
		if (why) *why = QStringLiteral("not an arrangement: %1").arg(error.errorString());
		return out;
	}
	const QJsonObject root = document.object();
	if (root.value(QStringLiteral("format")).toString() != QLatin1String(FORMAT)) {
		if (why) *why = QStringLiteral("not an arrangement");
		return out;
	}
	/* A later version may add what this cannot build; it says so rather than guessing. */
	if (root.value(QStringLiteral("version")).toInt() != VERSION) {
		if (why) *why = QStringLiteral("an arrangement of a version this BeerSSH does not read");
		return out;
	}

	for (const QJsonValue &value : root.value(QStringLiteral("windows")).toArray()) {
		const QJsonObject object = value.toObject();
		window_t window;
		const QJsonArray at = object.value(QStringLiteral("geometry")).toArray();
		if (at.size() == 4) {
			const QRect rect(at.at(0).toInt(), at.at(1).toInt(), at.at(2).toInt(), at.at(3).toInt());
			if (rect.width() > 0 && rect.height() > 0 && rect.width() < MAX_EXTENT &&
			    rect.height() < MAX_EXTENT && qAbs(rect.x()) < MAX_EXTENT && qAbs(rect.y()) < MAX_EXTENT)
				window.geometry = rect;
		}
		window.maximized = object.value(QStringLiteral("maximized")).toBool();
		window.full_screen = object.value(QStringLiteral("full_screen")).toBool();
		window.terminals_panel = object.value(QStringLiteral("terminals_panel")).toBool();
		window.shape = object.value(QStringLiteral("shape")).toString();
		for (const QJsonValue &name : object.value(QStringLiteral("terminals")).toArray()) {
			const QString text = name.toString();
			if (!text.isEmpty() && !window.terminals.contains(text)) window.terminals.append(text);
		}
		window.current = object.value(QStringLiteral("current")).toString();
		/* A window with nothing in it is nothing to build. */
		if (!window.terminals.isEmpty()) out.windows.append(window);
	}
	return out;
}
