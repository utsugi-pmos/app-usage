// SPDX-License-Identifier: LGPL-2.0-or-later

#include "calibrator.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

namespace {

// Measured, not estimated: five points at the 30 s default window, plus a 45 s
// baseline, a 45 s control and about two seconds of settling per step. The bar
// is driven by the clock rather than by parsing output, because calibrate's
// progress lines are for a person reading a terminal and turning them into a
// machine interface would make its prose an API.
const int EXPECTED_SECONDS = 4 * 60 + 15;

QString configPath()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
	       + QStringLiteral("/app-usage/calibration.ini");
}

// The root is overridable, and that is not decoration. calibrate learned this
// the hard way: its charger check used a hardcoded path, so its harness could
// only pass where no charger exists -- which is to say, anywhere except the one
// machine it is written for. The same applies here, where "the button is
// correctly hidden" and "the button is broken" look identical from outside.
//
// APP_USAGE_POWER_SUPPLY is for tests only; nothing sets it in normal use.
QString powerSupplyRoot()
{
	const QByteArray env = qgetenv("APP_USAGE_POWER_SUPPLY");
	return env.isEmpty() ? QStringLiteral("/sys/class/power_supply/")
	                     : QString::fromLocal8Bit(env);
}

bool chargerAttached()
{
	for (const char *supply : {"pm8150b-charger",
	                           "tcpm-source-psy-c440000.spmi:pmic@0:typec@1500"}) {
		QFile f(powerSupplyRoot() + QLatin1String(supply) + QStringLiteral("/online"));
		if (f.open(QIODevice::ReadOnly) && f.readAll().trimmed() == "1")
			return true;
	}
	return false;
}

} // namespace

Calibrator::Calibrator(QObject *parent)
    : QObject(parent)
{
	refresh();

	// The charger comes and goes while the screen is open, and the button has
	// to follow it. Slow on purpose: this is four small reads and nobody plugs
	// a phone in twice a second.
	auto *timer = new QTimer(this);
	timer->setInterval(3000);
	connect(timer, &QTimer::timeout, this, &Calibrator::refresh);
	timer->start();
}

Calibrator::~Calibrator()
{
	// The calibration leaves the backlight at zero while it runs and puts it
	// back at the end. Killing the process without letting it restore would
	// leave someone holding a phone with a black screen, so on shutdown it gets
	// a TERM and a moment to run its own cleanup.
	if (m_process && m_process->state() != QProcess::NotRunning) {
		m_process->terminate();
		m_process->waitForFinished(4000);
	}
}

void Calibrator::refresh()
{
	const bool wasScreen = m_screenKnown;
	const bool wasCpu = m_cpuKnown;
	const bool wasPossible = m_possible;
	const QString wasReason = m_blockedReason;

	m_screenKnown = false;
	m_cpuKnown = false;
	if (QFileInfo::exists(configPath())) {
		QSettings s(configPath(), QSettings::IniFormat);
		m_screenKnown = s.contains(QStringLiteral("screen/full_mw"));
		m_cpuKnown = s.contains(QStringLiteral("cpu/a55_factor"));
	}

	m_blockedReason.clear();
	if (m_running) {
		m_possible = false;
	} else if (QStandardPaths::findExecutable(QStringLiteral("app-usage-calibrate")).isEmpty()) {
		m_possible = false;
		m_blockedReason = tr("app-usage-calibrate is not installed.");
	} else if (chargerAttached()) {
		m_possible = false;
		m_blockedReason = tr("Unplug the charger first: with the cable in, the "
		                     "battery barely moves and there is nothing to measure.");
	} else {
		m_possible = true;
	}

	if (wasScreen != m_screenKnown || wasCpu != m_cpuKnown
	    || wasPossible != m_possible || wasReason != m_blockedReason)
		Q_EMIT stateChanged();
}

void Calibrator::setStatus(const QString &s, double progress)
{
	m_status = s;
	m_progress = progress;
	Q_EMIT progressChanged();
}

bool Calibrator::start()
{
	if (m_running || !m_possible)
		return false;

	const QString program = QStandardPaths::findExecutable(
	    QStringLiteral("app-usage-calibrate"));
	if (program.isEmpty())
		return false;

	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);

	m_clock = new QElapsedTimer;
	m_clock->start();

	auto *tick = new QTimer(m_process);
	tick->setInterval(500);
	connect(tick, &QTimer::timeout, this, [this]() {
		const double done = double(m_clock->elapsed()) / 1000.0 / EXPECTED_SECONDS;
		// Capped just short of full: a bar that sits at 100 % while something is
		// still running is a bar that has stopped telling the truth.
		setStatus(m_status, qMin(0.98, done));
	});
	tick->start();

	connect(m_process, &QProcess::finished, this,
	        [this](int code, QProcess::ExitStatus status) {
		const QString output = QString::fromUtf8(m_process->readAll());
		m_running = false;
		m_process->deleteLater();
		m_process = nullptr;
		delete m_clock;
		m_clock = nullptr;

		bool ok = (status == QProcess::NormalExit && code == 0);
		QString message;
		if (!ok) {
			// calibrate exits with its reason on the last line, and that reason is
			// worth more than "it failed": it says whether a charger appeared, or
			// the baseline drifted, or the phone was not still.
			const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
			message = lines.isEmpty() ? tr("Calibration failed.") : lines.last().trimmed();
		} else {
			refresh();
			// Success is not "the process exited 0", it is "the file the daemon
			// reads now has a screen section". calibrate refuses to write when the
			// control baseline drifted, and exits 0 having said so.
			if (!m_screenKnown) {
				ok = false;
				message = tr("The run finished but wrote nothing: the idle baseline "
				             "drifted, so the numbers would have been built on sand.");
			} else {
				message = tr("Calibrated. Restarting the collector.");
				// The daemon reads the file once, at startup.
				QProcess::startDetached(QStringLiteral("systemctl"),
				                        {QStringLiteral("--user"),
				                         QStringLiteral("restart"),
				                         QStringLiteral("app-usaged.service")});
			}
		}
		setStatus(message, ok ? 1.0 : 0.0);
		Q_EMIT runningChanged();
		Q_EMIT stateChanged();
		Q_EMIT finished(ok, message);
	});

	m_running = true;
	setStatus(tr("Measuring. Do not touch the phone: the screen goes dark and "
	             "steps through five brightness levels."), 0.0);
	Q_EMIT runningChanged();
	Q_EMIT stateChanged();

	m_process->start(program, {QStringLiteral("--only"), QStringLiteral("brightness")});
	return true;
}

void Calibrator::cancel()
{
	if (!m_process || m_process->state() == QProcess::NotRunning)
		return;
	// terminate, not kill: calibrate restores the backlight in its own cleanup,
	// and a KILL would leave the panel at whatever level the last step set --
	// possibly zero, which reads as a dead phone.
	m_process->terminate();
}
