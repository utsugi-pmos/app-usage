// SPDX-License-Identifier: LGPL-2.0-or-later

#include "launcher.h"

#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

Launcher::Launcher(QObject *parent)
    : QObject(parent)
{
	// kcmshell6 FIRST, and the order is not a guess -- it is measured. On this
	// phone, with no settings window open beforehand:
	//
	//   kcmshell6 kcm_mobile_power              -> window [kcm_mobile_power]
	//                                              titled "Energy". No errors.
	//   plasma-settings -m kcm_mobile_power     -> "unable to find module
	//   plasma-settings -m kcm_mobile_power -s     kcm_mobile_power", and a
	//                                              window titled "Settings",
	//                                              i.e. the module LIST.
	//
	// which is galling, because plasma-settings is the native settings app here
	// and `plasma-settings -l` lists kcm_mobile_power quite happily. It just
	// cannot open it. It stays as the fallback because landing someone in the
	// settings list is still better than a button that does nothing.
	//
	// Deliberately NOT the module's own .desktop file either: on this phone it
	// reads `Exec=systemsettings kcm_mobile_power` and systemsettings is not
	// installed, so honouring it would open nothing at all.
	struct Candidate {
		const char *program;
		QStringList arguments;
	};
	const Candidate candidates[] = {
		{"kcmshell6",       {QStringLiteral("kcm_mobile_power")}},
		{"plasma-settings", {QStringLiteral("-m"), QStringLiteral("kcm_mobile_power")}},
	};

	for (const Candidate &c : candidates) {
		const QString path = QStandardPaths::findExecutable(QLatin1String(c.program));
		if (!path.isEmpty()) {
			m_program = path;
			m_arguments = c.arguments;
			break;
		}
	}
}

bool Launcher::openEnergySettings()
{
	if (m_program.isEmpty() || m_opening)
		return false;

	// Detached: the settings app outlives this one, and a battery monitor should
	// not be holding a child process open for as long as someone reads a
	// different program's screen.
	if (!QProcess::startDetached(m_program, m_arguments))
		return false;

	// A timer rather than something cleverer, because there is nothing to watch:
	// the child is detached, so it has no exit to wait for, and asking the
	// compositor whether a window appeared would mean talking to KWin for a
	// progress spinner.
	//
	// 2.5 s is measured, not picked: kcmshell6 put a window on screen in 1.9 s
	// on three runs out of three, and that figure already includes the polling
	// overhead, so the real launch is a little quicker. Long enough to cover it,
	// short enough that a busy button never outlives the thing it is waiting for
	// by more than a moment.
	m_opening = true;
	Q_EMIT openingChanged();
	QTimer::singleShot(2500, this, [this]() {
		m_opening = false;
		Q_EMIT openingChanged();
	});
	return true;
}
