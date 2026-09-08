// SPDX-License-Identifier: LGPL-2.0-or-later

#include "usage.h"

#include "appinfo.h"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>

namespace {

QString stateDir()
{
	const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
	return (base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/state") : base)
	       + QStringLiteral("/app-usage");
}

// A phone's per-app CPU over a day spans milliseconds to tens of minutes.
// Printing one unit for all of it turns the bottom of the list into a column of
// zeroes, which reads as "did nothing" when it means "below the resolution
// somebody picked".
QString duration(double seconds)
{
	if (seconds >= 3600)
		return UsageModel::tr("%1 h").arg(seconds / 3600.0, 0, 'f', 1);
	if (seconds >= 60)
		return UsageModel::tr("%1 min").arg(seconds / 60.0, 0, 'f', 1);
	if (seconds >= 1)
		return UsageModel::tr("%1 s").arg(seconds, 0, 'f', 1);
	if (seconds > 0)
		return UsageModel::tr("%1 ms").arg(seconds * 1000.0, 0, 'f', 0);
	return QString();
}

} // namespace

UsageModel::UsageModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_store(stateDir())
{
	reload();
}

void UsageModel::setPeriod(const QString &p)
{
	if (p == m_period)
		return;
	m_period = p;
	Q_EMIT periodChanged();
	reload();
}

void UsageModel::setShowBackground(bool show)
{
	if (show == m_showBackground)
		return;
	m_showBackground = show;
	Q_EMIT showBackgroundChanged();
	beginResetModel();
	applyFilter();
	endResetModel();
	Q_EMIT reloaded();
}

void UsageModel::applyFilter()
{
	m_visible.clear();
	for (int i = 0; i < m_rows.size(); ++i) {
		// The screen and the system rows are never filtered out. They are not
		// applications, so "show only applications" cannot be about them, and
		// dropping them would make the remaining percentages look like they add
		// up to the whole battery when they do not.
		if (m_showBackground || !m_rows.at(i).background
		    || m_rows.at(i).kind != Store::App)
			m_visible.append(i);
	}
}

void UsageModel::reload()
{
	const QDateTime now = QDateTime::currentDateTimeUtc();
	QDateTime since = now.addSecs(-24 * 3600);
	if (m_period == QLatin1String("1h"))
		since = now.addSecs(-3600);
	else if (m_period == QLatin1String("7d"))
		since = now.addDays(-7);

	const QVector<Store::Row> rows = m_store.load(since);

	QHash<QString, Item> apps;
	QSet<QDateTime> measuredHours;
	double screenMj = 0, systemMj = 0;
	m_measured = 0;
	m_modelled = 0;
	m_elapsedMs = 0;
	m_appCpu = 0;
	for (const Store::Row &row : rows) {
		switch (row.kind) {
		case Store::App: {
			Item &item = apps[row.id];
			item.id = row.id;
			item.kind = Store::App;
			item.mj += row.mj;
			item.cpuUsec += row.cpuUsec;
			item.gpuNsec += row.gpuNsec;
			break;
		}
		case Store::Screen:   screenMj += row.mj;   break;
		case Store::System:   systemMj += row.mj;   break;
		case Store::Measured:
			m_measured += row.mj;
			// Each 'measured' row is one hour that carries a measurement. How many
			// there are decides whether the residual can even be looked at: with
			// twenty minutes of sampling the percentage is noise to two decimals.
			measuredHours.insert(row.hour);
			break;
		case Store::Elapsed: m_elapsedMs += row.mj; break;
		case Store::Modelled:
			m_modelled += row.mj;
			m_appCpu += row.cpuUsec;
			break;
		}
	}

	beginResetModel();
	m_rows.clear();
	for (auto it = apps.begin(); it != apps.end(); ++it) {
		it->name = AppInfo::displayName(it->id);
		it->icon = AppInfo::iconName(it->id);
		it->background = AppInfo::category(it->id) != AppInfo::Drawer;
		m_rows.append(*it);
	}

	// The screen and the system go in the same list as the applications rather
	// than in a separate header, because the question this answers is "where did
	// my battery go" and those two are usually most of the answer. Android does
	// the same. They sort on merit like everything else instead of being pinned
	// to the top, so a phone whose panel really was idle does not show a screen
	// row above the app that actually drained it.
	if (screenMj > 0) {
		Item item;
		item.kind = Store::Screen;
		item.name = tr("Screen");
		item.icon = QStringLiteral("preferences-system-brightness-lock");
		item.background = false;
		item.mj = screenMj;
		m_rows.append(item);
	}
	if (systemMj > 0) {
		Item item;
		item.kind = Store::System;
		item.name = tr("System");
		item.icon = QStringLiteral("preferences-system");
		item.background = false;
		item.mj = systemMj;
		m_rows.append(item);
	}

	std::sort(m_rows.begin(), m_rows.end(), [](const Item &a, const Item &b) {
		return a.mj > b.mj;
	});
	applyFilter();
	endResetModel();

	// The number phase 1 exists for, on the screen rather than only in a log:
	// how far the model's account of the applications sits from what the gauge
	// saw leave the battery. It is what decides whether phase 2 gets built, so
	// it is shown always, not only when it looks bad.
	if (m_measured > 0 && m_appCpu > 0) {
		const double off = 100.0 * (m_modelled - m_measured) / m_measured;
		// The hour count is part of the sentence, not a footnote. A residual
		// computed over twenty minutes of sampling is noise carrying a percent
		// sign, and nobody can tell that from a good one by looking at the
		// number alone. Saying what it rests on is what makes it readable.
		// The time REALLY measured, not the hours that contain it: the daemon
		// only records while unplugged, so an hour in the store may carry only
		// four minutes of measurement inside it.
		const double seconds = m_elapsedMs / 1000.0;
		m_residual = tr("%1 measured, %2 mW average: model %3 J for %4 of app "
		                "CPU, against %5 J real (%6 %)")
		                 .arg(seconds > 0 ? duration(seconds) : tr("?"))
		                 .arg(seconds > 0 ? m_measured / seconds : 0.0, 0, 'f', 0)
		                 .arg(m_modelled / 1000.0, 0, 'f', 2)
		                 .arg(duration(double(m_appCpu) / 1e6))
		                 .arg(m_measured / 1000.0, 0, 'f', 2)
		                 .arg(off, 0, 'f', 0);
	} else {
		m_residual.clear();
	}

	// Without a calibration there is no screen row and "system" is zero, and the
	// list will silently look like the applications account for the whole
	// battery. Saying so is cheaper than having someone believe it. See the
	// README for why the ORDER is still right regardless.
	m_calibrated = screenMj > 0 || systemMj > 0;
	if (m_rows.isEmpty() && !QDir(m_store.dir()).exists())
		// Same distinction the report makes: "nothing recorded yet" and "there
		// is nowhere to record it" look identical on screen, and only one of
		// them is fixed by waiting.
		m_note = tr("Cannot read %1. It is not that there is no data yet: there "
		            "is nowhere to keep it.").arg(m_store.dir());
	else if (m_rows.isEmpty())
		m_note = tr("No data yet. The daemon only measures while unplugged: "
		            "leave the charger off for a while and come back.");
	else if (!m_calibrated)
		m_note = tr("Not calibrated: the order between applications is right, "
		            "but the screen and the system's own use are not separated "
		            "out.");
	else
		m_note.clear();
	Q_EMIT reloaded();
}

int UsageModel::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : int(m_visible.size());
}

QVariant UsageModel::data(const QModelIndex &index, int role) const
{
	if (index.row() < 0 || index.row() >= m_visible.size())
		return QVariant();
	const Item &item = m_rows.at(m_visible.at(index.row()));

	// Percentages are of what the gauge MEASURED, not of the modelled sum, so
	// that when the model fails to explain the battery the rows visibly fail to
	// reach 100 % instead of being quietly renormalised to hide it.
	const double denom = m_measured > 0 ? m_measured : 1.0;

	switch (role) {
	case NameRole:       return item.name;
	case IconRole:       return item.icon;
	case ShareRole:      return qBound(0.0, item.mj / denom, 1.0);
	case PercentRole:    return item.mj * 100.0 / denom;
	case JoulesRole:     return item.mj / 1000.0;
	case CpuTextRole:    return duration(double(item.cpuUsec) / 1e6);
	case GpuTextRole:    return duration(double(item.gpuNsec) / 1e9);
	case KindRole:       return Store::kindName(item.kind);
	case BackgroundRole: return item.background;
	default:             return QVariant();
	}
}

QHash<int, QByteArray> UsageModel::roleNames() const
{
	return {
		{NameRole,       "name"},
		{IconRole,       "iconName"},
		{ShareRole,      "share"},
		{PercentRole,    "percent"},
		{JoulesRole,     "joules"},
		{CpuTextRole,    "cpuText"},
		{GpuTextRole,    "gpuText"},
		{KindRole,       "kind"},
		{BackgroundRole, "background"},
	};
}
