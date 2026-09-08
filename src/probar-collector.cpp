// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Diagnostic driver for the collector. Takes two sweeps a few seconds apart and
// prints everything they saw, so the parsing can be checked against a phone
// that is actually running rather than against the author's expectations.
//
// This exists because compiling collector.cpp proves nothing about whether the
// sysfs paths are right, whether the cgroup root resolves, or whether the
// fdinfo lines are being split on the character they are actually separated by.
// Every one of those is a silent empty result, not an error.
//
//   ./probar-collector [seconds] [--forzar]      default 5 s
//
// Note that attribute() will refuse the interval while the phone is plugged in,
// which is correct and is itself worth seeing: see the guard in attribute().
//
// --forzar lies about the charger so the split arithmetic runs anyway. The
// millijoules it prints are WORTHLESS -- with USB attached the battery barely
// circulates and the gauge is measuring nothing -- but the shares, the ordering
// and the size of the "system" bucket are still worth looking at, and it is a
// great deal cheaper than discovering a division by zero halfway through an
// unplugged calibration session.

#include "collector.h"

#include <QCoreApplication>
#include <QDir>
#include <QThread>

#include <cstdio>

static void dumpSystem(const char *label, const SystemSample &s)
{
	std::printf("--- %s -----------------------------------------\n", label);
	std::printf("  when          %s\n", qPrintable(s.when.toString(Qt::ISODate)));
	std::printf("  power         %.1f mW\n", s.powerMw);
	std::printf("  onBattery     %s\n", s.onBattery ? "yes" : "NO (charger attached)");
	std::printf("  brightness    %d / %d  (screen %s)\n", s.brightness, s.maxBrightness,
	            s.screenOn ? "on" : "off");
	std::printf("  totalCpuUsec  %llu\n", static_cast<unsigned long long>(s.totalCpuUsec));
	for (auto it = s.timeInState.constBegin(); it != s.timeInState.constEnd(); ++it)
		std::printf("  policy%-2d      %d frequencies\n", it.key(), int(it.value().size()));
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	int seconds = 5;
	bool force = false;
	for (int i = 1; i < argc; ++i) {
		const QString arg = QString::fromLatin1(argv[i]);
		if (arg == QLatin1String("--forzar"))
			force = true;
		else
			seconds = arg.toInt();
	}
	if (seconds <= 0)
		seconds = 5;

	Calibration cal = Calibration::load(
	    QDir::homePath() + QStringLiteral("/.config/app-usage/calibracion.ini"));
	std::printf("calibration   cpu:%s screen:%s",
	            cal.cpuKnown ? "yes" : "NO", cal.screenKnown ? "yes" : "NO");
	if (cal.any())
		std::printf("  a55=%.3f a76=%.3f screen=%.0f+%.0f mW",
		            cal.a55Factor, cal.a76Factor, cal.screenBaseMw, cal.screenFullMw);
	std::printf("\n\n");

	Collector c(cal);

	SystemSample s1, s2;
	QVector<AppSample> a1, a2;

	if (!c.sweep(&s1, &a1)) {
		std::fprintf(stderr, "the first sweep failed\n");
		return 1;
	}
	dumpSystem("sweep 1", s1);
	std::printf("  applications  %d\n", int(a1.size()));

	QThread::sleep(seconds);

	if (!c.sweep(&s2, &a2)) {
		std::fprintf(stderr, "the second sweep failed\n");
		return 1;
	}
	dumpSystem("sweep 2", s2);
	std::printf("  applications  %d\n\n", int(a2.size()));

	// What moved between the two sweeps. An app whose counters did not advance
	// is not printed: on an idle phone that is most of them, and a wall of zeros
	// hides the handful that matter.
	QHash<QString, quint64> prev;
	for (const AppSample &a : a1)
		prev.insert(a.appId, a.cpuUsec);

	std::printf("--- what moved ----------------------------------------\n");
	int moved = 0;
	for (const AppSample &a : a2) {
		const quint64 d = a.cpuUsec - qMin(a.cpuUsec, prev.value(a.appId, 0));
		if (d == 0 && a.gpuNsec == 0)
			continue;
		++moved;
		std::printf("  %-42s %8.1f ms", qPrintable(a.appId), double(d) / 1000.0);
		if (a.gpuCycles)
			std::printf("   gpu %llu cycles", static_cast<unsigned long long>(a.gpuCycles));
		std::printf("\n");
	}
	if (moved == 0)
		std::printf("  (nothing; either the phone is very still or the parsing is wrong)\n");

	std::printf("\n--- the split ------------------------------------------\n");
	if (force && !(s1.onBattery && s2.onBattery)) {
		s1.onBattery = s2.onBattery = true;
		std::printf("  (--forzar: the millijoules below are WORTHLESS, only the split matters)\n");
	}
	IntervalResult r;
	if (!c.attribute(s1, a1, s2, a2, &r)) {
		std::printf("  interval rejected. This is correct if a charger is attached:\n"
		            "  with USB plugged in the battery does not circulate and the measurement is worthless.\n");
		return 0;
	}
	std::printf("  measured energy   %.1f mJ\n", r.energyMj);
	std::printf("  model (apps)      %.1f mJ\n", r.modelledAppMj);
	std::printf("  cpu apps/total    %.0f / %.0f ms  (%.0f %% of the machine)\n",
	            double(r.appCpuUsec) / 1000.0, double(r.totalCpuUsec) / 1000.0,
	            r.totalCpuUsec > 0 ? 100.0 * double(r.appCpuUsec) / double(r.totalCpuUsec) : 0.0);
	std::printf("  screen            %.1f mJ\n", r.screenMj);
	std::printf("  unattributed      %.1f mJ  (%.0f %%)\n", r.unattributedMj,
	            r.energyMj > 0 ? 100.0 * r.unattributedMj / r.energyMj : 0.0);
	if (!cal.cpuKnown)
		std::printf("  (without CPU calibration the cap is not applied: the split is\n"
		            "   proportional but \"system\" will come out zero)\n");
	for (auto it = r.perApp.constBegin(); it != r.perApp.constEnd(); ++it)
		std::printf("  %-42s %8.1f mJ\n", qPrintable(it.key()), it.value());
	return 0;
}
