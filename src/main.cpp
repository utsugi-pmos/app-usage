// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The daemon, and the report that reads what it wrote.
//
// It has to be a daemon rather than something that reads on demand, and the
// reason is not efficiency: a cgroup disappears when its application exits, and
// takes its counters with it. Nothing that starts up after the fact can find
// out what a program that closed an hour ago cost you.
//
//   app-usaged --demonio                 sweep, attribute, store. The service.
//   app-usaged --informe [--periodo 24h] print what the store holds
//
// The report exists before the QML screen on purpose. It is the same pipeline
// the screen will draw, so anything wrong with the numbers is wrong here too,
// and it can be read over ssh without a compositor.

#include "appinfo.h"
#include "collector.h"
#include "store.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <algorithm>
#include <csignal>
#include <cstdio>

namespace {

// Written by the signal handler, read by the timer. Nothing else may go in a
// handler: flushing from inside one would mean allocating and touching a QMap
// while the main loop is halfway through the same structures.
volatile sig_atomic_t g_stop = 0;

void onSignal(int)
{
	g_stop = 1;
}

QString stateDir()
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
	return (base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : base)
	       + QStringLiteral("/app-usage");
}

QString configPath()
{
	return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
	       + QStringLiteral("/app-usage/calibration.ini");
}

int runDaemon(const QString &dir, int periodSec, int flushSec, int keepDays, bool force)
{
	Calibration cal = Calibration::load(configPath());
	Collector collector(cal);
	Store store(dir);

	std::fprintf(stderr, "app-usaged: store at %s, one sweep every %d s\n",
	             qPrintable(store.dir()), periodSec);
	if (!cal.screenKnown || !cal.cpuKnown)
		std::fprintf(stderr,
		             "app-usaged: calibration %s. The order between applications is\n"
		             "         still correct -- the factors cancel out in the\n"
		             "         proportional split -- but %s\n"
		             "         Try: app-usage-calibrate --solo brillo  (3 min, unplugged)\n",
		             cal.any() ? "incomplete" : "absent",
		             !cal.screenKnown && !cal.cpuKnown
		                 ? "there will be no screen row and \"system\" will come out zero."
		             : !cal.screenKnown ? "there will be no screen row."
		                                : "\"system\" is not capped by the CPU model.");
	const int purged = store.purge(keepDays);
	if (purged)
		std::fprintf(stderr, "app-usaged: %d expired days deleted\n", purged);

	SystemSample prevSys;
	QVector<AppSample> prevApps;
	bool havePrev = false;
	QDateTime lastFlush = QDateTime::currentDateTimeUtc();
	QDate lastPurgeDay = lastFlush.date();

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	while (!g_stop) {
		SystemSample sys;
		QVector<AppSample> apps;
		if (collector.sweep(&sys, &apps)) {
			if (havePrev) {
				// Forcing exists so the pipeline can be exercised on a phone that
				// is plugged in, which is most of the time while developing. The
				// millijoules it stores are not worth reading; the point is that
				// the plumbing runs.
				if (force) {
					prevSys.onBattery = true;
					sys.onBattery = true;
				}
				IntervalResult r;
				if (collector.attribute(prevSys, prevApps, sys, apps, &r))
					store.add(r);
			}
			prevSys = sys;
			prevApps = apps;
			havePrev = true;
		}

		const QDateTime now = QDateTime::currentDateTimeUtc();
		if (lastFlush.secsTo(now) >= flushSec) {
			if (!store.flush())
				std::fprintf(stderr, "app-usaged: %s\n", qPrintable(store.errorString()));
			lastFlush = now;
		}
		if (now.date() != lastPurgeDay) {
			store.purge(keepDays);
			lastPurgeDay = now.date();
		}

		// Sleep in short slices so a SIGTERM is answered in about a second
		// instead of after a whole sweep period. systemd's default stop timeout
		// is generous, but a service that takes half a minute to die makes every
		// restart feel broken.
		for (int i = 0; i < periodSec && !g_stop; ++i)
			QThread::sleep(1);
	}

	std::fprintf(stderr, "app-usaged: stopping, flushing the store\n");
	if (!store.flush()) {
		std::fprintf(stderr, "app-usaged: %s\n", qPrintable(store.errorString()));
		return 1;
	}
	return 0;
}

int runReport(const QString &dir, const QString &period)
{
	QDateTime since;
	const QDateTime now = QDateTime::currentDateTimeUtc();
	if (period == QLatin1String("24h"))
		since = now.addSecs(-24 * 3600);
	else if (period == QLatin1String("7d"))
		since = now.addDays(-7);
	else if (period == QLatin1String("1h"))
		since = now.addSecs(-3600);
	else {
		std::fprintf(stderr, "unknown period: %s (valid: 1h, 24h or 7d)\n",
		             qPrintable(period));
		return 2;
	}

	Store store(dir);

	// "No data yet, wait for a sweep" is a reassuring sentence, and it has to be
	// EARNED. If the directory does not exist -- wrong --state, a home that was
	// never written to, a typo -- then telling someone to wait is telling them
	// to wait forever for a thing that will not happen. The two look identical
	// from the outside, which is exactly why they have to be told apart here.
	if (!QDir(dir).exists()) {
		std::fprintf(stderr,
		             "%s does not exist, and I could not create it.\n"
		             "It is not that there is no data yet: it is that there is\n"
		             "nowhere to keep it. Check the path and the permissions.\n",
		             qPrintable(dir));
		return 1;
	}

	const QVector<Store::Row> rows = store.load(since);
	if (rows.isEmpty()) {
		std::printf("No data in %s for the last %s.\n"
		            "The daemon saves every few minutes; if it has just started,\n"
		            "wait for a sweep to pass.\n",
		            qPrintable(store.dir()), qPrintable(period));
		return 0;
	}

	QHash<QString, double> perApp;
	QHash<QString, quint64> cpuPerApp;
	double screen = 0, system = 0, measured = 0, modelled = 0;
	quint64 appCpu = 0;
	QSet<QDateTime> measuredHours;
	double elapsedMs = 0;
	for (const Store::Row &row : rows) {
		switch (row.kind) {
		case Store::App:
			perApp[row.id] += row.mj;
			cpuPerApp[row.id] += row.cpuUsec;
			break;
		case Store::Screen:   screen += row.mj;   break;
		case Store::System:   system += row.mj;   break;
		case Store::Measured:
			measured += row.mj;
			// Each 'measured' row is one hour that carries a measurement.
			measuredHours.insert(row.hour);
			break;
		case Store::Modelled:
			modelled += row.mj;
			appCpu += row.cpuUsec;
			break;
		case Store::Elapsed: elapsedMs += row.mj; break;
		}
	}

	QVector<QPair<double, QString>> ranked;
	for (auto it = perApp.constBegin(); it != perApp.constEnd(); ++it)
		ranked.append({it.value(), it.key()});
	std::sort(ranked.begin(), ranked.end(),
	          [](const QPair<double, QString> &a, const QPair<double, QString> &b) {
		          return a.first > b.first;
	          });

	// Percentages are of what the gauge measured, not of the modelled sum. If
	// the two disagree the rows will visibly fail to reach 100 %, and that gap
	// is the residual this whole phase exists to expose.
	double shownTotal = screen + system;
	for (const auto &p : ranked)
		shownTotal += p.first;
	const double total = measured > 0 ? measured : shownTotal;
	auto pct = [total](double mj) { return total > 0 ? 100.0 * mj / total : 0.0; };

	// A phone's per-app CPU over an hour ranges from a couple of milliseconds to
	// tens of minutes. Printing whole seconds turns the entire bottom of the
	// list into "0 s", which reads as "did nothing" when it means "below the
	// resolution I chose to print".
	auto duration = [](double seg) {
		if (seg >= 3600)
			return QStringLiteral("%1 h").arg(seg / 3600.0, 0, 'f', 1);
		if (seg >= 60)
			return QStringLiteral("%1 min").arg(seg / 60.0, 0, 'f', 1);
		return QStringLiteral("%1 s").arg(seg, 0, 'f', 0);
	};

	auto cpuText = [](quint64 usec) {
		const double s = double(usec) / 1e6;
		if (s >= 60)
			return QStringLiteral("%1 min").arg(s / 60.0, 0, 'f', 1);
		if (s >= 1)
			return QStringLiteral("%1 s").arg(s, 0, 'f', 1);
		return QStringLiteral("%1 ms").arg(s * 1000.0, 0, 'f', 0);
	};

	std::printf("Consumption over the last %s  (%.1f J measured from the battery)\n\n",
	            qPrintable(period), measured / 1000.0);
	if (screen > 0)
		std::printf("  %-30s %8.2f J  %5.1f %%\n", "Screen", screen / 1000.0, pct(screen));
	for (const auto &p : ranked) {
		// The same drawer/background split the screen shows, as a suffix rather
		// than a column: most rows on this phone are background services, and a
		// column of repeated words would be wider than the numbers it sits next
		// to.
		const bool bg = AppInfo::category(p.second) != AppInfo::Drawer;
		std::printf("  %-30s %8.2f J  %5.1f %%  %10s CPU%s\n",
		            qPrintable(AppInfo::displayName(p.second)), p.first / 1000.0, pct(p.first),
		            qPrintable(cpuText(cpuPerApp.value(p.second))),
		            bg ? "  (background)" : "");
	}
	if (system > 0)
		std::printf("  %-30s %8.2f J  %5.1f %%\n", "System", system / 1000.0, pct(system));

	// THE NUMBER PHASE 1 EXISTS FOR, printed always and not only when it looks
	// bad. How far the energy model's account of the applications sits from what
	// the gauge actually saw leave the battery is what decides whether phase 2 --
	// eBPF over sched_switch, and a kernel rebuilt with CONFIG_DEBUG_INFO_BTF --
	// is worth doing. Hiding it under a threshold would hide the decision.
	if (measured > 0 && appCpu > 0) {
		// The hour count goes FIRST, just like on the screen: it is what
		// decides whether the percentage can even be read. Over twenty minutes
		// of sampling it is noise carrying a percent sign.
		// The time REALLY measured, not the number of hours that contain it: the
		// daemon only records while unplugged, so an hour in the store may carry
		// only four minutes of measurement inside it, and saying "1 h" would take
		// for granted a sampling fifteen times larger than there actually was.
		const double segundos = elapsedMs / 1000.0;
		if (segundos > 0)
			std::printf("\n  %s measured, %.0f mW average\n",
			            qPrintable(duration(segundos)), measured / segundos);
		std::printf("  the model attributes %.2f J to %.0f s of application CPU,\n"
		            "  against %.2f J real: %+.0f %%\n",
		            modelled / 1000.0, double(appCpu) / 1e6, measured / 1000.0,
		            100.0 * (modelled - measured) / measured);
	}

	// If the modelled rows do not reach the measured total, say so instead of
	// letting the percentages quietly fail to add up. Without a calibration this
	// is expected: the screen row does not exist and "system" cannot be capped.
	const double gap = total - shownTotal;
	if (total > 0 && qAbs(gap) > 0.01 * total)
		std::printf("\n  unexplained: %.2f J (%.1f %%)\n", gap / 1000.0, 100.0 * gap / total);
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("app-usaged"));

	QCommandLineParser p;
	p.setApplicationDescription(
	    "Splits the energy that leaves the battery between the applications.");
	p.addHelpOption();
	QCommandLineOption daemonOpt(QStringList() << QStringLiteral("demonio"),
	                             "sample and save, without exiting");
	QCommandLineOption reportOpt(QStringList() << QStringLiteral("informe"),
	                             "print what is stored");
	QCommandLineOption periodOpt(QStringList() << QStringLiteral("periodo"),
	                             "1h, 24h or 7d (default 24h)", "period",
	                             QStringLiteral("24h"));
	QCommandLineOption everyOpt(QStringList() << QStringLiteral("cada"),
	                            "seconds between sweeps (default 30)", "sec",
	                            QStringLiteral("30"));
	QCommandLineOption keepOpt(QStringList() << QStringLiteral("dias"),
	                           "retention days (default 8)", "days",
	                           QStringLiteral("8"));
	QCommandLineOption flushOpt(QStringList() << QStringLiteral("vaciar-cada"),
	                            "seconds between writes to disk (default 300)",
	                            "sec", QStringLiteral("300"));
	QCommandLineOption forceOpt(QStringList() << QStringLiteral("forzar"),
	                            "save even with a charger attached. For testing only, "
	                            "and requires --state: the joules are worthless");
	QCommandLineOption stateOpt(QStringList() << QStringLiteral("state"),
	                            "where it is stored (default ~/.local/state/app-usage)",
	                            "dir");
	p.addOption(daemonOpt);
	p.addOption(reportOpt);
	p.addOption(periodOpt);
	p.addOption(everyOpt);
	p.addOption(keepOpt);
	p.addOption(flushOpt);
	p.addOption(forceOpt);
	p.addOption(stateOpt);
	p.process(app);

	const QString dir = p.isSet(stateOpt) ? p.value(stateOpt) : stateDir();

	// --forzar WITHOUT --state is refused, and this is not pedantry: it is the
	// exact mistake that happened here. Forcing writes intervals measured with a
	// charger attached, where the battery barely circulates and the joules mean
	// nothing -- and it wrote them straight into the real store, which then had
	// to be found and wiped by hand. Making the flag name its own directory
	// makes that impossible rather than merely discouraged.
	if (p.isSet(forceOpt) && !p.isSet(stateOpt)) {
		std::fprintf(stderr,
		             "--forzar needs --state <dir>.\n"
		             "\n"
		             "It forces intervals measured WITH a charger, where the battery\n"
		             "barely circulates and the joules mean nothing. Writing them\n"
		             "into the real store contaminates it without anyone noticing:\n"
		             "the numbers come out, and they are a lie.\n"
		             "\n"
		             "  app-usaged --demonio --forzar --state /tmp/test\n"
		             "  app-usaged --informe --state /tmp/test\n");
		return 2;
	}

	if (p.isSet(reportOpt))
		return runReport(dir, p.value(periodOpt));
	if (p.isSet(daemonOpt))
		return runDaemon(dir, p.value(everyOpt).toInt(), p.value(flushOpt).toInt(),
		                 p.value(keepOpt).toInt(), p.isSet(forceOpt));

	std::fprintf(stderr, "Use --demonio or --informe. --help for the rest.\n");
	return 2;
}
