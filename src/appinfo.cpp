// SPDX-License-Identifier: LGPL-2.0-or-later

#include "appinfo.h"

#include <QHash>
#include <QSettings>
#include <QStandardPaths>

namespace {

struct Entry {
	QString name;
	QString icon;
	AppInfo::Category category = AppInfo::Background;
};

// One lookup per id for the life of the process. The list redraws on every
// period change and every reload, and a .desktop lookup is a directory search
// plus a file parse; doing that per frame is how a list gets janky.
const Entry &lookup(const QString &appId)
{
	static QHash<QString, Entry> cache;
	const auto hit = cache.constFind(appId);
	if (hit != cache.constEnd())
		return hit.value();

	Entry e;
	e.name = appId;
	const QString file = QStandardPaths::locate(
	    QStandardPaths::ApplicationsLocation, appId + QStringLiteral(".desktop"));
	if (!file.isEmpty()) {
		QSettings desktop(file, QSettings::IniFormat);
		desktop.beginGroup(QStringLiteral("Desktop Entry"));

		const QString n = desktop.value(QStringLiteral("Name")).toString();
		if (!n.isEmpty())
			e.name = n;
		e.icon = desktop.value(QStringLiteral("Icon")).toString();

		// The three keys the launcher itself honours, in the order the spec
		// gives them.
		//
		// NoDisplay means "I ship a .desktop so I can be D-Bus activated or own
		// a MIME type, but I am not an application anyone opens". On this phone
		// that is the Discover notifier, the Xwayland video bridge and the GTK
		// portal -- three things that would otherwise sit in the list looking
		// like apps.
		//
		// Hidden means the entry was deleted by the user; the spec says treat it
		// as though it were not installed at all.
		//
		// NotShowIn/OnlyShowIn are the desktop-environment filter. A GNOME-only
		// entry is not in this phone's launcher, so it is not an app here even
		// though its file exists.
		const bool noDisplay = desktop.value(QStringLiteral("NoDisplay"), false).toBool();
		const bool hidden = desktop.value(QStringLiteral("Hidden"), false).toBool();

		const QString notShowIn = desktop.value(QStringLiteral("NotShowIn")).toString();
		const QString onlyShowIn = desktop.value(QStringLiteral("OnlyShowIn")).toString();
		const QLatin1String here("KDE");
		const bool excluded =
		    notShowIn.split(QLatin1Char(';'), Qt::SkipEmptyParts).contains(here)
		    || (!onlyShowIn.isEmpty()
		        && !onlyShowIn.split(QLatin1Char(';'), Qt::SkipEmptyParts).contains(here));

		if (!noDisplay && !hidden && !excluded)
			e.category = AppInfo::Drawer;
	}
	return *cache.insert(appId, e);
}

} // namespace

QString AppInfo::displayName(const QString &appId)
{
	return lookup(appId).name;
}

QString AppInfo::iconName(const QString &appId)
{
	return lookup(appId).icon;
}

AppInfo::Category AppInfo::category(const QString &appId)
{
	return lookup(appId).category;
}
