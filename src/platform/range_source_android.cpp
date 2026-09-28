#include "platform/range_source_android.h"

#include "model/font_download.h"

#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QPointer>
#include <QThread>

namespace {

const char *const BSSH_HTTP_CLASS = "se/vibes/beerssh/BsshHttp";

QString last_error() {
	QJniEnvironment env;
	env.checkAndClearExceptions();

	const QJniObject reason = QJniObject::callStaticObjectMethod(
	        BSSH_HTTP_CLASS, "lastError", "()Ljava/lang/String;");
	const QString text = reason.isValid() ? reason.toString() : QString();
	return text.isEmpty() ? QStringLiteral("the download failed") : text;
}

/*
 * bssh_range_source over BsshHttp. Each request runs on a thread of its
 * own, because Android refuses network access on the UI thread, and its
 * answer is delivered back on the UI thread -- where bssh_font_download
 * expects it, as it gets QNetworkReply's -- unless this source has gone
 * in the meantime, when it is dropped rather than delivered to a picker
 * that no longer exists.
 */
class android_range_source : public QObject, public bssh_range_source {
public:
	void size(const QUrl &url, std::function<void(qint64, QString)> done) override {
		const QString address = url.toString(QUrl::FullyEncoded);

		run([address]() {
			const QJniObject jaddress = QJniObject::fromString(address);
			const jlong length = QJniObject::callStaticMethod<jlong>(
			        BSSH_HTTP_CLASS, "size", "(Ljava/lang/String;)J", jaddress.object<jstring>());
			return length < 0 ? std::make_pair(qint64(-1), last_error())
			                  : std::make_pair(qint64(length), QString());
		}, [done](const std::pair<qint64, QString> &result) {
			done(result.first, result.second);
		});
	}

	void fetch(const QUrl &url, qint64 from, qint64 length,
	           std::function<void(QByteArray, QString)> done) override {
		const QString address = url.toString(QUrl::FullyEncoded);

		run([address, from, length]() {
			const QJniObject jaddress = QJniObject::fromString(address);
			const QJniObject bytes = QJniObject::callStaticObjectMethod(
			        BSSH_HTTP_CLASS, "fetch", "(Ljava/lang/String;JJ)[B",
			        jaddress.object<jstring>(), jlong(from), jlong(length));
			if (!bytes.isValid()) return std::make_pair(QByteArray(), last_error());

			QJniEnvironment env;
			const jbyteArray array = bytes.object<jbyteArray>();
			const jsize count = env->GetArrayLength(array);
			QByteArray out(count, Qt::Uninitialized);
			env->GetByteArrayRegion(array, 0, count, reinterpret_cast<jbyte *>(out.data()));
			return std::make_pair(out, QString());
		}, [done](const std::pair<QByteArray, QString> &result) {
			done(result.first, result.second);
		});
	}

private:
	template <typename Work, typename Deliver>
	void run(Work work, Deliver deliver) {
		const QPointer<QObject> alive(this);

		QThread *thread = QThread::create([alive, work, deliver]() {
			const auto result = work();
			QMetaObject::invokeMethod(qApp, [alive, deliver, result]() {
				if (alive) deliver(result);
			}, Qt::QueuedConnection);
		});
		QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
		thread->start();
	}
};

} /* namespace */

void bssh_install_android_range_source() {
	bssh_set_range_source_factory([]() -> bssh_range_source * {
		return new android_range_source();
	});
}
