// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Opening Plasma's own energy page, instead of building a second one.
//
// The battery's state, health and technology already have a screen on this
// phone, and it is a better one than a copy would be: it is the one that gets
// updated with Plasma. Reimplementing it would mean either duplicating it and
// letting the two drift, or embedding org.kde.kcm.power.mobile.private -- a
// module whose name says out loud that it is not an API to build on.
//
// So this app does the one thing that page does not: where the battery went,
// per application. For everything else it points at the real thing.

#pragma once

#include <QObject>

class Launcher : public QObject {
	Q_OBJECT

public:
	explicit Launcher(QObject *parent = nullptr);

	// Opens the phone's Energy settings. Returns false when nothing could be
	// started, so the button can be hidden rather than becoming a control that
	// does nothing when pressed.
	Q_INVOKABLE bool openEnergySettings();

	// True from the tap until the other window has had time to appear.
	//
	// It exists because without it the button is a control that visibly does
	// nothing: measured on this phone, kcmshell6 takes 1.9 s to put a window on
	// screen, three runs out of three. Two seconds of a button that swallowed
	// your tap and did not react is exactly how an app teaches you it is broken.
	Q_PROPERTY(bool opening READ opening NOTIFY openingChanged)
	bool opening() const { return m_opening; }

	// Whether there is anything to open at all. Checked once at startup.
	Q_PROPERTY(bool available READ available CONSTANT)
	bool available() const { return !m_program.isEmpty(); }

Q_SIGNALS:
	void openingChanged();

private:
	QString m_program;
	QStringList m_arguments;
	bool m_opening = false;
};
