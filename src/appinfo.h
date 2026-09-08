// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Turning a cgroup id into something a person recognises, and deciding whether
// it is a thing they launched or a thing that was always running.
//
// The collector produces ids like "org.kde.spectacle" or "plasma-plasmashell",
// because that is what systemd names the scope. Half of them have a .desktop
// file with a proper name and an icon; the other half are daemons, portals and
// helpers that were never meant to appear in a launcher.
//
// Those are shown under their raw id rather than hidden or lumped together, and
// that is the whole point of the screen: the thing draining your battery is
// very often exactly the daemon nobody put an icon on.

#pragma once

#include <QString>

namespace AppInfo {

// What kind of thing this is, from the user's point of view.
enum Category {
	// It has an entry in the launcher: the user chose to run it.
	Drawer,
	// Everything else -- session services, autostarted agents, D-Bus activated
	// helpers, plumbing. Not hidden, just marked, so a background daemon eating
	// the battery is still visible AND still recognisable as not-an-app.
	Background,
};

// The Name= from the .desktop file, or the id unchanged when there is none.
// Cached: a list redraw asks for the same forty names over and over.
QString displayName(const QString &appId);

// The Icon= from the .desktop file, or an empty string. Callers are expected to
// have a fallback -- most entries on this phone will not resolve.
QString iconName(const QString &appId);

// Drawer when a .desktop exists in the applications directories and nothing in
// it says "do not show me". That is not an approximation of the launcher's
// rule, it IS the launcher's rule -- the XDG one -- so the two cannot disagree
// about what counts as an app.
//
// Measured against the 32 live cgroups of this phone: the only unit it calls a
// Drawer app that was actually launched from the drawer is Spectacle, and the
// only other two it accepts (Screenglaze, app-usage) do have launcher entries.
// The three .desktop files carrying NoDisplay -- the Discover notifier, the
// Xwayland bridge and the GTK portal -- are all correctly left in Background,
// as are the nine @autostart agents and the nine D-Bus activated services,
// none of which have a .desktop at all.
Category category(const QString &appId);

} // namespace AppInfo
