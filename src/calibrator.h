// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Running the screen calibration from the app, and knowing when it is worth
// offering.
//
// Only the brightness ladder, never the CPU sweep. The ladder takes about four
// minutes and needs nothing but a phone nobody is touching; the CPU sweep takes
// eighteen, has to visit every OPP with the governor pinned, and is a thing you
// start deliberately from a terminal, not something to put behind a button
// someone might press while waiting for a bus.
//
// The ladder is also the half that buys the most: the panel is the biggest
// single consumer on a phone and it is not attributable to any application, so
// without it the screen's energy is silently spread over whatever happened to
// be running.

#pragma once

#include <QObject>
#include <QString>

class QProcess;
class QElapsedTimer;

class Calibrator : public QObject {
	Q_OBJECT

	// What the stored calibration covers. Two flags because the two halves are
	// measured separately: a screen-only run must not look like a full one.
	Q_PROPERTY(bool screenKnown READ screenKnown NOTIFY stateChanged)
	Q_PROPERTY(bool cpuKnown READ cpuKnown NOTIFY stateChanged)

	// Whether it can run right now. The gauge only measures anything with the
	// charger off, so offering the button while plugged in would be offering a
	// four-minute wait for a file that calibrate would refuse to write.
	Q_PROPERTY(bool possible READ possible NOTIFY stateChanged)
	Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY stateChanged)

	Q_PROPERTY(bool running READ running NOTIFY runningChanged)
	Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
	Q_PROPERTY(QString status READ status NOTIFY progressChanged)

public:
	explicit Calibrator(QObject *parent = nullptr);
	~Calibrator() override;

	bool screenKnown() const { return m_screenKnown; }
	bool cpuKnown() const { return m_cpuKnown; }
	bool possible() const { return m_possible; }
	QString blockedReason() const { return m_blockedReason; }
	bool running() const { return m_running; }
	double progress() const { return m_progress; }
	QString status() const { return m_status; }

	// Re-reads the stored calibration and the charger. Cheap: four small files.
	Q_INVOKABLE void refresh();

	Q_INVOKABLE bool start();
	Q_INVOKABLE void cancel();

Q_SIGNALS:
	void stateChanged();
	void runningChanged();
	void progressChanged();
	void finished(bool ok, const QString &message);

private:
	QProcess *m_process = nullptr;
	QElapsedTimer *m_clock = nullptr;
	bool m_screenKnown = false;
	bool m_cpuKnown = false;
	bool m_possible = false;
	bool m_running = false;
	double m_progress = 0;
	QString m_blockedReason;
	QString m_status;

	void setStatus(const QString &s, double progress);
};
