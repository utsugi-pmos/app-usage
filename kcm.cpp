// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The System Settings module. Battery usage is a reader, so this is the whole
// of it: it builds the three models the QML needs and reloads on show. There is
// no save() and no needsSave -- nothing here writes anything, so the KCM frame
// shows no Apply button. The models are the same classes the standalone window
// links, so the two front-ends cannot drift apart.

#include "calibrator.h"
#include "launcher.h"
#include "usage.h"

#include <KPluginFactory>
#include <KQuickConfigModule>

class KCMAppUsage : public KQuickConfigModule
{
	Q_OBJECT

	Q_PROPERTY(UsageModel *usage READ usage CONSTANT)
	Q_PROPERTY(Launcher *launcher READ launcher CONSTANT)
	Q_PROPERTY(Calibrator *calibrator READ calibrator CONSTANT)

public:
	KCMAppUsage(QObject *parent, const KPluginMetaData &data)
	    : KQuickConfigModule(parent, data)
	    , m_usage(new UsageModel(this))
	    , m_launcher(new Launcher(this))
	    , m_calibrator(new Calibrator(this))
	{
	}

	UsageModel *usage() const { return m_usage; }
	Launcher *launcher() const { return m_launcher; }
	Calibrator *calibrator() const { return m_calibrator; }

	// Reload every time the module is shown: the daemon has been writing while
	// Settings was on another page, and the store is cheap to re-read.
	void load() override { m_usage->reload(); }

private:
	UsageModel *const m_usage;
	Launcher *const m_launcher;
	Calibrator *const m_calibrator;
};

K_PLUGIN_CLASS_WITH_JSON(KCMAppUsage, "kcm_app_usage.json")

#include "kcm.moc"
