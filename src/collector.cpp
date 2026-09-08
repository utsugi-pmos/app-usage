// SPDX-License-Identifier: LGPL-2.0-or-later

#include "collector.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>

#include <unistd.h>

namespace {

const char *QG = "/sys/class/power_supply/qcom_qg/";
const char *CPUFREQ = "/sys/devices/system/cpu/cpufreq/";
const char *BACKLIGHT = "/sys/class/backlight/backlight/";
const char *ENERGY_MODEL = "/sys/kernel/debug/energy_model/";

// The two performance domains of the sm7150. policy0 is the six A55s, policy6
// the two A76s.
const int POLICIES[] = {0, 6};

// EVERY read of a kernel file in this program goes through here, and it has to.
//
// kernfs -- sysfs, procfs and cgroupfs alike -- does not report a usable
// st_size. cgroup and proc files claim 0; sysfs files claim one full page.
// Measured on this phone: cpu.stat says 0 and holds 156 bytes, time_in_state
// says 4096 and holds 134.
//
// QTextStream believes that number, and both lies break it in a different and
// silent way. Over a cgroup cpu.stat, atEnd() is true before the first read, so
// the loop body never runs: every app reports zero CPU and the screen looks
// like an idle phone. Over time_in_state, atEnd() never becomes true, so the
// loop spins forever returning empty lines and pins a core at 100 % -- which is
// a memorable thing for a battery monitor to do.
//
// QIODevice::readAll() is the one API that reads incrementally when the size is
// unknown, so it is correct for both. Do not reintroduce QTextStream here.
QString readFile(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll()).trimmed();
}

QStringList readLines(const QString &path)
{
	const QString s = readFile(path);
	if (s.isEmpty())
		return QStringList();
	return s.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// "usage_usec 33959167006" out of a cpu.stat. Returns 0 when the key is absent,
// which is indistinguishable from a genuine zero -- acceptable here because a
// cgroup that has used no CPU has nothing to attribute either way.
quint64 readKeyedValue(const QString &path, QLatin1String key)
{
	const QStringList lines = readLines(path);
	for (const QString &line : lines) {
		const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (parts.size() == 2 && parts[0] == key)
			return parts[1].toULongLong();
	}
	return 0;
}

qint64 readNumber(const QString &path, qint64 fallback = 0)
{
	bool ok = false;
	const qint64 v = readFile(path).toLongLong(&ok);
	return ok ? v : fallback;
}

// The user's cgroup root. The uid is looked up rather than assumed: on this
// phone it is 10000, not the 1000 every example on the internet uses, and
// hardcoding 1000 yields an empty app list that looks like "nothing is running"
// instead of an error.
QString userCgroupRoot()
{
	return QStringLiteral("/sys/fs/cgroup/user.slice/user-%1.slice/user@%1.service")
	        .arg(getuid());
}

// "app-org.kde.spectacle.service"                        -> "org.kde.spectacle"
// "app-poconav@addb588d0d174be8943e275752ed692b.service"  -> "poconav"
// "app-org.kde.kclockd\x2dautostart@autostart.service"    -> "org.kde.kclockd-autostart"
// "app-dbus\x2d:1.1\x2dorg.bluez.obex.slice"              -> "org.bluez.obex"
//
// systemd escapes characters that are not valid in a unit name, so the hyphen
// in a desktop id arrives as \x2d and has to be put back or the .desktop lookup
// fails and the app shows up under its raw unit name.
//
// The suffix list is not decoration: 8 of the 36 units under app.slice on this
// phone are .slice directories, because a D-Bus activated service gets a slice
// of its own. Reading them is right -- cgroup v2 accounting is hierarchical, so
// the slice already includes its children and there is nothing to descend into
// -- but they arrive wrapped in a "dbus-:1.1-" prefix that is systemd's
// bookkeeping, not part of any application's name.
QString appIdFromUnit(const QString &unit)
{
	QString s = unit;
	for (QLatin1String suffix : {QLatin1String(".service"), QLatin1String(".scope"),
	                             QLatin1String(".slice"), QLatin1String(".socket")}) {
		if (s.endsWith(suffix)) {
			s.chop(suffix.size());
			break;
		}
	}
	if (s.startsWith(QLatin1String("app-")))
		s.remove(0, 4);

	// Undo systemd's escaping before anything else looks at the string.
	static const QRegularExpression esc(QStringLiteral("\\\\x([0-9a-fA-F]{2})"));
	QRegularExpressionMatch m;
	while ((m = esc.match(s)).hasMatch())
		s.replace(m.capturedStart(), m.capturedLength(),
		          QChar(m.captured(1).toInt(nullptr, 16)));

	// Now that the escaping is undone, drop the D-Bus activation prefix.
	static const QRegularExpression dbusPrefix(QStringLiteral("^dbus-:[0-9.]+-"));
	s.remove(dbusPrefix);

	// Drop the systemd instance suffix: everything after the last '@'.
	const int at = s.lastIndexOf(QLatin1Char('@'));
	if (at > 0)
		s.truncate(at);
	return s;
}

} // namespace

// ---------------------------------------------------------------- calibration

Calibration Calibration::load(const QString &path)
{
	Calibration c;
	if (!QFileInfo::exists(path))
		return c;

	QSettings s(path, QSettings::IniFormat);
	c.a55Factor = s.value(QStringLiteral("cpu/a55_factor"), 1.0).toDouble();
	c.a76Factor = s.value(QStringLiteral("cpu/a76_factor"), 1.0).toDouble();
	c.screenBaseMw = s.value(QStringLiteral("screen/base_mw"), 0.0).toDouble();
	c.screenFullMw = s.value(QStringLiteral("screen/full_mw"), 0.0).toDouble();

	// Presence of the KEY, not a plausible value: calibrar writes only the
	// sections it actually measured, so a missing [cpu] means "not measured"
	// and has to stay distinguishable from "measured and came out at 1.0".
	c.screenKnown = s.contains(QStringLiteral("screen/full_mw"))
	                && (c.screenBaseMw > 0 || c.screenFullMw > 0);
	// A factor of zero would silently zero out the whole CPU term, so treat a
	// malformed file as "not measured" rather than as calibration saying the
	// CPU is free.
	c.cpuKnown = s.contains(QStringLiteral("cpu/a55_factor"))
	             && c.a55Factor > 0 && c.a76Factor > 0;
	if (!c.cpuKnown) {
		c.a55Factor = 1.0;
		c.a76Factor = 1.0;
	}
	return c;
}

// ------------------------------------------------------------------ collector

Collector::Collector(const Calibration &cal)
    : m_cal(cal)
{
	loadEnergyModel();
}

bool Collector::loadEnergyModel()
{
	// debugfs is root-only, so this runs once at startup through sudo -n and the
	// result is kept for the life of the process. It never changes: the table is
	// built from the device tree at boot.
	QProcess p;
	p.start(QStringLiteral("sudo"),
	        {QStringLiteral("-n"), QStringLiteral("sh"), QStringLiteral("-c"),
	         QStringLiteral("for d in %1*/ps:*/; do echo \"$d $(cat $d/frequency) "
	                        "$(cat $d/power)\"; done").arg(QLatin1String(ENERGY_MODEL))});
	p.waitForFinished(5000);

	const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'));
	for (const QString &line : lines) {
		const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (parts.size() != 3)
			continue;
		// ".../energy_model/cpu0/ps:300000/" -> cpu0
		const QStringList seg = parts[0].split(QLatin1Char('/'), Qt::SkipEmptyParts);
		if (seg.size() < 2)
			continue;
		const QString cpu = seg.at(seg.size() - 2);
		const int policy = (cpu == QLatin1String("cpu0")) ? 0 : 6;
		m_energyModel[policy].insert(parts[1].toULongLong(), parts[2].toDouble() / 1000.0);
	}

	if (m_energyModel.isEmpty())
		qWarning("app-usaged: no energy model (debugfs needs sudo -n). "
		         "The split between applications still holds; the breakdown "
		         "between CPU and screen does not.");
	return !m_energyModel.isEmpty();
}

void Collector::rescanDrmFds()
{
	// The expensive one: walking every /proc/<pid>/fdinfo/* looking for a
	// "drm-" line costs 79 ms with ~600 open fds. At a 1 Hz sweep that would be
	// roughly 13 mW -- a visible slice of a phone's idle draw, spent by the tool
	// whose whole job is to tell you what is spending your battery.
	//
	// So it runs rarely, and only the handful of files it finds (about ten) get
	// read on every sweep, which costs well under a millisecond.
	m_drmFds.clear();
	const QDir proc(QStringLiteral("/proc"));
	const QStringList pids = proc.entryList(QStringList() << QStringLiteral("[0-9]*"),
	                                        QDir::Dirs | QDir::NoSymLinks);
	for (const QString &pidStr : pids) {
		const int pid = pidStr.toInt();
		const QString fdinfoDir = QStringLiteral("/proc/%1/fdinfo").arg(pid);
		QDir d(fdinfoDir);
		if (!d.exists())
			continue;
		QStringList found;
		const QStringList fds = d.entryList(QDir::Files | QDir::NoSymLinks);
		for (const QString &fd : fds) {
			const QString path = fdinfoDir + QLatin1Char('/') + fd;
			QFile f(path);
			if (!f.open(QIODevice::ReadOnly))
				continue;
			// The DRM keys are at the top of the file; reading a small prefix is
			// enough and avoids pulling in whole fdinfo bodies.
			if (f.read(512).contains("drm-driver"))
				found << path;
		}
		if (!found.isEmpty())
			m_drmFds.insert(pid, found);
	}
	m_drmScanned = QDateTime::currentDateTimeUtc();
}

bool Collector::sweep(SystemSample *sys, QVector<AppSample> *apps)
{
	sys->when = QDateTime::currentDateTimeUtc();

	// --- the one real measurement ------------------------------------------
	const double uA = qAbs(double(readNumber(QLatin1String(QG) + QStringLiteral("current_now"))));
	const double uV = double(readNumber(QLatin1String(QG) + QStringLiteral("voltage_now")));
	sys->powerMw = uA * uV / 1e9;

	// "Discharging" is necessary but not sufficient: with USB attached the
	// gauge reports "Not charging" while the charger quietly feeds the system,
	// and the battery barely circulates. Any interval measured like that is
	// noise, so the charger is checked directly.
	sys->onBattery = readFile(QLatin1String(QG) + QStringLiteral("status"))
	                 == QLatin1String("Discharging");
	for (const char *supply : {"pm8150b-charger",
	                           "tcpm-source-psy-c440000.spmi:pmic@0:typec@1500"}) {
		if (readNumber(QStringLiteral("/sys/class/power_supply/%1/online")
		               .arg(QLatin1String(supply)), 0) == 1)
			sys->onBattery = false;
	}

	sys->brightness = int(readNumber(QLatin1String(BACKLIGHT) + QStringLiteral("brightness")));
	sys->maxBrightness = int(readNumber(QLatin1String(BACKLIGHT)
	                                    + QStringLiteral("max_brightness"), 4095));
	sys->screenOn = sys->brightness > 0;

	// --- how the clusters spent their time ---------------------------------
	for (int policy : POLICIES) {
		const QString path = QStringLiteral("%1policy%2/stats/time_in_state")
		                     .arg(QLatin1String(CPUFREQ)).arg(policy);
		const QStringList lines = readLines(path);
		for (const QString &line : lines) {
			const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
			if (parts.size() == 2)
				sys->timeInState[policy].insert(parts[0].toULongLong(), parts[1].toULongLong());
		}
	}

	// Total CPU time of the whole machine, apps and kernel threads alike. This
	// is what bounds how much of the measured energy the CPU term is allowed to
	// claim; without it the split would hand every last millijoule to whichever
	// apps happened to be running.
	sys->totalCpuUsec = readKeyedValue(QStringLiteral("/sys/fs/cgroup/cpu.stat"),
	                                   QLatin1String("usage_usec"));

	// --- CPU per application, from the cgroups systemd already made ---------
	apps->clear();
	QHash<QString, AppSample> byApp;
	// A cheap fingerprint of "which applications exist right now", accumulated
	// while the directories are being walked anyway. It is what tells the GPU
	// scan below that something launched or exited, without a second pass over
	// anything. Sensitive to the set, not to the order: entryList sorts.
	QString unitSignature;
	const QStringList slices = {QStringLiteral("app.slice"), QStringLiteral("session.slice")};
	for (const QString &slice : slices) {
		QDir d(userCgroupRoot() + QLatin1Char('/') + slice);
		const QStringList units = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
		unitSignature += units.join(QLatin1Char('\n'));
		unitSignature += QLatin1Char('\x1e');
		for (const QString &unit : units) {
			const quint64 usage = readKeyedValue(d.filePath(unit) + QStringLiteral("/cpu.stat"),
			                                     QLatin1String("usage_usec"));
			if (usage == 0)
				continue;

			const QString appId = appIdFromUnit(unit);
			// Several units can map to one app: a .service and a .scope, or two
			// instances of the same program. They are summed under one id so the
			// user sees one row per application, which is what they asked for.
			AppSample &s = byApp[appId];
			s.appId = appId;
			if (s.unit.isEmpty())
				s.unit = unit;
			s.cpuUsec += usage;
		}
	}

	// --- GPU per application ------------------------------------------------
	//
	// When to redo the expensive scan. Measured on this phone it costs 130 ms,
	// against well under a millisecond for rereading the ~30 files it finds, so
	// this decision is most of what the collector costs.
	//
	// A plain timer was the first attempt and it was wrong: at 60 s it fired on
	// every other 30 s sweep, which took the daemon to 50.8 ms per sweep --
	// nearly ten times what it needs, and precisely the mistake the design notes
	// call "the only part of this that would show up in the battery".
	//
	// So it rescans on the two things that actually change the answer, both of
	// which are already known for free by the time we get here:
	//
	//   - a process holding cached DRM fds died, so its rows are stale;
	//   - the set of running applications changed, so a new one may have opened
	//     the GPU.
	//
	// The slow timer stays underneath as a backstop, at ten minutes rather than
	// one, for the case both miss: a long-lived process that opens its first DRM
	// fd without its cgroup changing.
	bool rescan = !m_drmScanned.isValid()
	              || m_drmScanned.secsTo(sys->when) > 600
	              || unitSignature != m_drmUnits;
	if (!rescan) {
		for (auto it = m_drmFds.constBegin(); it != m_drmFds.constEnd(); ++it) {
			if (!QFileInfo::exists(QStringLiteral("/proc/%1").arg(it.key()))) {
				rescan = true;
				break;
			}
		}
	}
	if (rescan) {
		rescanDrmFds();
		m_drmUnits = unitSignature;
	}

	QHash<int, QString> pidToApp;
	// Map the cached DRM pids onto apps through their cgroup, which is the same
	// grouping the CPU numbers use. Reading one small file per DRM pid is cheap;
	// there are about ten of them.
	for (auto it = m_drmFds.constBegin(); it != m_drmFds.constEnd(); ++it) {
		const QString cg = readFile(QStringLiteral("/proc/%1/cgroup").arg(it.key()));
		if (cg.isEmpty())
			continue;
		const int slash = cg.lastIndexOf(QLatin1Char('/'));
		if (slash < 0)
			continue;
		pidToApp.insert(it.key(), appIdFromUnit(cg.mid(slash + 1)));
	}

	for (auto it = m_drmFds.constBegin(); it != m_drmFds.constEnd(); ++it) {
		const QString appId = pidToApp.value(it.key());
		if (appId.isEmpty())
			continue;
		// A process opens the DRM device several times; each fd is a separate
		// client with its own counters, so they are summed.
		quint64 ns = 0, cycles = 0;
		for (const QString &path : it.value()) {
			const QStringList lines = readLines(path);
			for (const QString &line : lines) {
				// fdinfo separates key from value with a tab, but the driver pads
				// with spaces too, so the value is taken as "everything after the
				// colon" rather than trusting a single separator.
				if (line.startsWith(QLatin1String("drm-engine-gpu:")))
					ns += line.section(QLatin1Char(':'), 1).remove(QLatin1String("ns")).trimmed().toULongLong();
				else if (line.startsWith(QLatin1String("drm-cycles-gpu:")))
					cycles += line.section(QLatin1Char(':'), 1).trimmed().toULongLong();
			}
		}
		AppSample &s = byApp[appId];
		s.appId = appId;
		s.gpuNsec += ns;
		s.gpuCycles += cycles;
	}

	for (auto it = byApp.constBegin(); it != byApp.constEnd(); ++it)
		apps->append(it.value());
	return true;
}

double Collector::meanCpuMw(int policy, const QHash<quint64, quint64> &before,
                            const QHash<quint64, quint64> &after) const
{
	// Weighted mean of the energy-model power over the frequencies the cluster
	// actually visited during the interval. time_in_state counts in 10 ms
	// jiffies and is per-policy, not per-CPU.
	const auto &table = m_energyModel.value(policy);
	if (table.isEmpty())
		return 0;

	double weighted = 0;
	quint64 total = 0;
	for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
		const quint64 ticks = it.value() - before.value(it.key(), 0);
		if (ticks == 0 || !table.contains(it.key()))
			continue;
		weighted += double(ticks) * table.value(it.key());
		total += ticks;
	}
	if (total == 0)
		return 0;

	const double mw = weighted / double(total);
	return mw * (policy == 0 ? m_cal.a55Factor : m_cal.a76Factor);
}

bool Collector::attribute(const SystemSample &prevSys, const QVector<AppSample> &prevApps,
                          const SystemSample &nowSys, const QVector<AppSample> &nowApps,
                          IntervalResult *out)
{
	const qint64 ms = prevSys.when.msecsTo(nowSys.when);

	// Three reasons to throw an interval away rather than guess at it.
	//
	// 1. A charger was attached at either end. The battery does not circulate
	//    and the gauge reading is meaningless -- this is the same trap that
	//    makes the phase 0 calibration require an unplugged phone.
	// 2. The interval is far longer than the sampling period, which means the
	//    machine was suspended. The counters advanced by an unknown amount
	//    spread over an unknown wall time, so a rate cannot be recovered.
	// 3. Time went backwards (clock adjustment, NTP step).
	if (!prevSys.onBattery || !nowSys.onBattery)
		return false;
	if (ms <= 0 || ms > 10 * 60 * 1000)
		return false;

	out->start = prevSys.when;
	out->end = nowSys.when;

	// The measured energy of the interval: the average of the two endpoint
	// power readings over the elapsed time. This is the number everything else
	// is a share of, and the only one that is not a model.
	const double meanMw = (prevSys.powerMw + nowSys.powerMw) / 2.0;
	out->energyMj = meanMw * (double(ms) / 1000.0);

	// The screen is charged as its own consumer, exactly the way Android does
	// it, and not spread across apps. It is usually the biggest single item, so
	// folding it into whoever happened to be in the foreground would drown every
	// real difference between applications.
	if (nowSys.screenOn && m_cal.screenKnown) {
		const double frac = nowSys.maxBrightness > 0
		                    ? double(nowSys.brightness) / double(nowSys.maxBrightness) : 0;
		const double screenMw = m_cal.screenBaseMw + frac * m_cal.screenFullMw;
		out->screenMj = qMin(screenMw * (double(ms) / 1000.0), out->energyMj);
	}

	// --- per-app weights ----------------------------------------------------
	QHash<QString, quint64> prevCpu, prevGpu;
	for (const AppSample &a : prevApps) {
		prevCpu.insert(a.appId, a.cpuUsec);
		prevGpu.insert(a.appId, a.gpuNsec);
	}

	QHash<QString, double> weights;
	double totalWeight = 0;
	const double cpuMw55 = meanCpuMw(0, prevSys.timeInState.value(0), nowSys.timeInState.value(0));
	const double cpuMw76 = meanCpuMw(6, prevSys.timeInState.value(6), nowSys.timeInState.value(6));

	// THE KNOWN WEAKNESS OF PHASE 1, stated plainly because it decides whether
	// phase 2 gets built:
	//
	// cpu.stat gives microseconds, not which core they ran on. An A76 at
	// 2.3 GHz costs 968.8 mW and an A55 at 300 MHz costs 13.8 mW -- a factor of
	// 70 -- so weighting every app's microseconds by the same figure is exactly
	// the mistake that makes Scaphandre useless on an asymmetric SoC.
	//
	// Averaging the two clusters is not a fix; it is an admission. What it does
	// buy is a number for the residual: if the modelled total tracks the gauge
	// across a real day, the approximation is good enough for "what is eating my
	// battery". If it does not, that is the evidence for spending a kernel
	// rebuild on CONFIG_DEBUG_INFO_BTF and doing this properly with eBPF over
	// sched_switch.
	const double cpuMwMean = (cpuMw55 + cpuMw76) > 0 ? (cpuMw55 + cpuMw76) / 2.0 : 1.0;

	// What the energy model is actually FOR here.
	//
	// It cannot separate two apps from each other -- the same coefficient
	// multiplies both, so it cancels in the proportional split. What it can do
	// is say how much of the measured energy went into the applications at all,
	// which is what stops the split from handing the modem's and the radios'
	// consumption to whichever app happened to be awake.
	//
	// The ceiling is built from the CPU time THE APPS THEMSELVES used, not from
	// the machine's. That distinction is the whole thing, and getting it wrong
	// was measured on this phone: over a 10 s interval the machine burned 13.2 s
	// of CPU while the user's applications accounted for 1.8 s of it. A ceiling
	// built from 13.2 s is seven times too generous, never binds, and hands the
	// apps 100 % of the measured energy -- so "system" reads exactly 0.0 mJ no
	// matter what the phone is really doing.
	//
	// That is not a cosmetic difference. The residual IS the deliverable of
	// phase 1: it is the number that decides whether phase 2 gets built. A
	// residual that is structurally pinned at zero cannot decide anything.
	//
	// Kernel threads, system.slice and everything else outside app.slice are not
	// applications, so their energy belongs in "system" by the same argument
	// that puts the modem there.
	quint64 dAppCpu = 0;

	for (const AppSample &a : nowApps) {
		// A cgroup that did not exist in the previous sweep starts its counters
		// at zero, so its absolute value IS its delta. A cgroup that vanished
		// takes its last interval with it -- an app that opens and closes
		// between two sweeps is invisible. That is the price of sampling, and it
		// is why the period is 30 s and not 5 minutes.
		const quint64 dCpu = a.cpuUsec - qMin(a.cpuUsec, prevCpu.value(a.appId, 0));
		const quint64 dGpu = a.gpuNsec - qMin(a.gpuNsec, prevGpu.value(a.appId, 0));
		if (dCpu == 0 && dGpu == 0)
			continue;

		// GPU nanoseconds are counted as busy time on a device whose power is
		// not in the kernel's energy model, so they carry the CPU's mean figure
		// as a stand-in until phase 0 measures a GPU coefficient. Deliberately
		// crude and deliberately visible.
		const double w = double(dCpu) * cpuMwMean + (double(dGpu) / 1000.0) * cpuMwMean;
		if (w <= 0)
			continue;
		weights.insert(a.appId, w);
		totalWeight += w;
		dAppCpu += dCpu;
		out->cpuUsec.insert(a.appId, dCpu);
		out->gpuNsec.insert(a.appId, dGpu);
	}

	const quint64 dTotalCpu = nowSys.totalCpuUsec
	                          - qMin(nowSys.totalCpuUsec, prevSys.totalCpuUsec);
	out->appCpuUsec = dAppCpu;
	out->totalCpuUsec = dTotalCpu;
	out->modelledAppMj = (double(dAppCpu) / 1e6) * cpuMwMean;

	// --- the split ----------------------------------------------------------
	// Everything the apps did not account for stays in its own bucket instead of
	// being spread over them. The modem, the wifi radio, the DSPs and the idle
	// floor all land here, and none of them belong to an application. A big
	// "system" share is information, not a bug -- it is the signal that the
	// model is missing something.
	//
	// The pool the apps share is capped by their own modelled energy, so an idle
	// phone burning 300 mW on its radios does not report that as "Firefox".
	//
	// The cap is only as good as the calibration behind it: uncalibrated, the
	// energy model's milliwatts are the device tree's dynamic-power figures with
	// no leakage, no L3 and no bus, and phase 0 exists to put a scale factor on
	// them. So the cap is applied only once a calibration has been loaded.
	// Without one, capping by an unscaled model would move energy into "system"
	// for a reason that is arithmetic rather than physical, and the residual --
	// the one number phase 1 is here to produce -- would be measuring the
	// missing calibration instead of the missing model.
	//
	// When there is no energy model at all -- debugfs unreadable -- the same
	// applies and the cap is dropped rather than set to zero, because a wrong
	// split is still more useful than an empty screen, and the missing model is
	// warned about at startup.
	double appEnergy = out->energyMj - out->screenMj;
	if (m_cal.cpuKnown && out->modelledAppMj > 0)
		appEnergy = qMin(appEnergy, out->modelledAppMj);
	if (totalWeight > 0 && appEnergy > 0) {
		for (auto it = weights.constBegin(); it != weights.constEnd(); ++it)
			out->perApp.insert(it.key(), appEnergy * (it.value() / totalWeight));
	}

	double attributed = out->screenMj;
	for (double mj : std::as_const(out->perApp))
		attributed += mj;
	out->unattributedMj = qMax(0.0, out->energyMj - attributed);
	return true;
}
