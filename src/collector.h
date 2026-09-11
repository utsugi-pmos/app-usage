// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The sampler: one sweep of every counter that says where the battery went,
// and the arithmetic that turns those counters into an energy share per app.
//
// See surya/tasks/018-battery-consumption-per-app.md for why each source
// was picked and what it costs to read. The short version:
//
//   - CPU per app comes from cgroup v2, because systemd already puts every
//     application in its own scope. 5.4 ms to sweep all 136 of them, against
//     12.5 ms to walk /proc, and the "application" unit comes pre-aggregated.
//   - GPU per app comes from DRM fdinfo. The PIDs that own DRM fds are cached:
//     rediscovering them every sweep costs 79 ms, which at 1 Hz would be ~13 mW
//     and therefore visible in the very measurement this is trying to make.
//   - Total power comes from the fuel gauge, and it is the only number here
//     that is a measurement rather than a model.

#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

// Raw per-application counters, as read. Deltas are computed against the
// previous sweep; the absolute values are meaningless on their own because a
// cgroup's counters start at zero when the app launches.
struct AppSample {
	QString unit;          // "app-org.kde.spectacle.service"
	QString appId;         // "org.kde.spectacle", pulled out of the unit name
	quint64 cpuUsec = 0;   // cpu.stat usage_usec, monotonic within one cgroup
	quint64 gpuNsec = 0;   // sum of drm-engine-gpu over the app's DRM clients
	quint64 gpuCycles = 0; // sum of drm-cycles-gpu, already frequency-normalised
};

// One sweep of everything that is not per-app.
struct SystemSample {
	QDateTime when;
	double powerMw = 0;        // |current_now| * voltage_now, the real thing
	bool onBattery = false;    // false while a charger feeds the system
	int brightness = 0;        // 0..max_brightness
	int maxBrightness = 4095;
	bool screenOn = false;
	QString foregroundApp;     // appId of the focused window, may be empty
	quint64 totalCpuUsec = 0;  // root cgroup cpu.stat: ALL CPU time, not just apps
	// time_in_state per policy, in 10 ms jiffies: freq kHz -> ticks
	QHash<int, QHash<quint64, quint64>> timeInState;
};

// What one interval produced: how much energy really left the battery, and how
// it was split.
struct IntervalResult {
	QDateTime start;
	QDateTime end;
	double energyMj = 0;              // measured, not modelled
	double screenMj = 0;              // charged to the screen, not to any app
	double unattributedMj = 0;        // the honest "system" bucket
	QHash<QString, double> perApp;    // appId -> mJ
	QHash<QString, quint64> cpuUsec;  // appId -> CPU microseconds this interval
	QHash<QString, quint64> gpuNsec;  // appId -> GPU nanoseconds this interval

	// The three numbers phase 1 exists to produce. Comparing modelledAppMj with
	// energyMj across a real day is the criterion that decides whether phase 2
	// (eBPF over sched_switch) is worth a kernel rebuild -- not whether the
	// screen looks plausible.
	double modelledAppMj = 0;   // what the energy model says the apps cost
	quint64 appCpuUsec = 0;     // CPU time inside app.slice and session.slice
	quint64 totalCpuUsec = 0;   // CPU time of the whole machine, apps included
};

// The calibration produced by phase 0. Without it the collector still runs and
// still produces correct SHARES -- the scale factors cancel out in the
// proportional split -- but the screen model needs real numbers.
struct Calibration {
	// TWO flags, not one, because the two halves are measured separately and
	// mean different things. `app-usage-calibrate --solo brillo` takes three
	// minutes and needs only a still phone; the CPU sweep takes eighteen and
	// has to visit every OPP. Treating a screen-only run as "calibrated" would
	// switch on the CPU ceiling with the device tree's unscaled milliwatts,
	// which measures the calibration that is missing rather than the model that
	// is missing.
	bool cpuKnown = false;
	bool screenKnown = false;

	double a55Factor = 1.0;      // measured mW / energy-model mW
	double a76Factor = 1.0;
	double screenBaseMw = 0;     // panel at minimum, over black
	double screenFullMw = 0;     // additional mW at full brightness

	bool any() const { return cpuKnown || screenKnown; }
	static Calibration load(const QString &path);
};

class Collector {
public:
	explicit Collector(const Calibration &cal);

	// Reads everything once. Cheap: the whole sweep is ~6 ms.
	bool sweep(SystemSample *sys, QVector<AppSample> *apps);

	// Turns two consecutive sweeps into an energy split. Returns false when the
	// interval must be thrown away rather than guessed at -- see the .cpp for
	// the three cases where that happens.
	bool attribute(const SystemSample &prevSys, const QVector<AppSample> &prevApps,
	               const SystemSample &nowSys, const QVector<AppSample> &nowApps,
	               IntervalResult *out);

private:
	Calibration m_cal;

	// PID -> DRM fdinfo paths. Rebuilt only when the process set changes, which
	// is what keeps the GPU read from costing 130 ms a sweep.
	QHash<int, QStringList> m_drmFds;
	QDateTime m_drmScanned;
	QString m_drmUnits;   // the app-unit set as of the last scan
	void rescanDrmFds();

	// Energy model, read once from debugfs at startup: policy -> kHz -> mW.
	QHash<int, QHash<quint64, double>> m_energyModel;
	bool loadEnergyModel();

	// Mean power of one busy CPU over an interval, weighted by how long the
	// cluster actually spent at each frequency. This is the approximation that
	// phase 2 would replace with eBPF; see attribute() for what it gets wrong.
	double meanCpuMw(int policy, const QHash<quint64, quint64> &before,
	                 const QHash<quint64, quint64> &after) const;
};
