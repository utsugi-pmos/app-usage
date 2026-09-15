// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The GUI, as its own binary.
//
// Separate from the daemon on purpose. The daemon is QtCore only and runs for
// days; linking QtQuick into it would pull the whole graphics stack into a
// process that never draws anything, and would make the thing whose job is to
// be cheap the largest resident on the list it prints.

#include "calibrator.h"
#include "launcher.h"
#include "usage.h"

#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QTimer>
#include <QTranslator>

int main(int argc, char **argv)
{
	// --captura <file>: draw one frame, write a PNG, exit.
	//
	// This exists because checking that the screen renders turned into a fight
	// with the compositor. Spectacle is single-instance and D-Bus activated, so
	// a stale `spectacle --dbus` swallows every later request and reports
	// success while writing nothing; and when it did fire, the panel was blanked
	// and it captured a white rectangle. None of that says anything about
	// whether the QML is right.
	//
	// With QT_QPA_PLATFORM=offscreen this needs no compositor, no unlocked
	// session and no lit panel, and it produces the same pixels the phone would
	// draw. Being able to look at the thing is worth twenty lines.
	QString grabTo;
	for (int i = 1; i < argc - 1; ++i) {
		if (QLatin1String(argv[i]) == QLatin1String("--capture"))
			grabTo = QString::fromLocal8Bit(argv[i + 1]);
	}

	QGuiApplication app(argc, argv);
	QGuiApplication::setApplicationName(QStringLiteral("app-usage"));
	QGuiApplication::setDesktopFileName(QStringLiteral("app-usage"));
	QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("app-usage")));

	// Source strings are English and the translations are loaded here, rather
	// than the app being written in Spanish and left that way. Both files are
	// tried: Qt's own for the strings inside QtQuick's controls, and ours.
	QTranslator qtTranslator;
	if (qtTranslator.load(QLocale(), QStringLiteral("qt"), QStringLiteral("_"),
	                      QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
		QCoreApplication::installTranslator(&qtTranslator);

	QTranslator translator;
	// The build embeds the .qm files under :/i18n; the install prefix copy is a
	// fallback for running from a source tree.
	if (translator.load(QLocale(), QStringLiteral("app-usage"), QStringLiteral("_"),
	                    QStringLiteral(":/i18n"))
	    || translator.load(QLocale(), QStringLiteral("app-usage"), QStringLiteral("_"),
	                       QStringLiteral("/usr/share/app-usage/i18n")))
		QCoreApplication::installTranslator(&translator);

	UsageModel usage;
	Launcher launcher;
	Calibrator calibrator;

	QQmlApplicationEngine engine;
	engine.rootContext()->setContextProperty(QStringLiteral("usage"), &usage);
	engine.rootContext()->setContextProperty(QStringLiteral("launcher"), &launcher);
	engine.rootContext()->setContextProperty(QStringLiteral("calibrator"), &calibrator);
	// Quit on a failed load rather than sitting there with no window: on a phone
	// there is no console in front of the user, so a silent failure looks like
	// the app simply not starting.
	QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
	                 []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);

	// By URL rather than loadFromModule(). App.qml is the window; the screen
	// itself is Body.qml, shared with the System Settings module so the two
	// front-ends cannot drift. (The old entry was a lowercase main.qml loaded by
	// URL because loadFromModule() looks up a TYPE and QML type names must start
	// with a capital; App.qml would load either way, and the URL is what
	// screenglaze next door already does.)
	engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AppUsage/App.qml")));

	if (!grabTo.isEmpty()) {
		const auto roots = engine.rootObjects();
		if (roots.isEmpty())
			return 1;
		auto *window = qobject_cast<QQuickWindow *>(roots.first());
		if (!window)
			return 1;
		// One turn of the loop before grabbing: the list is populated from the
		// store during construction, but the scene graph has not drawn yet and
		// grabbing immediately gives an empty window.
		QTimer::singleShot(1200, &app, [window, grabTo]() {
			const QImage shot = window->grabWindow();
			if (shot.isNull() || !shot.save(grabTo)) {
				qWarning("could not write %s", qPrintable(grabTo));
				QCoreApplication::exit(1);
				return;
			}
			QCoreApplication::quit();
		});
	}

	return app.exec();
}
