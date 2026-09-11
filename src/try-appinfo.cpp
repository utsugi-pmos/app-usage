// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Does the drawer/background split actually split?
//
// The rule is the XDG one -- a .desktop in the applications directories with
// nothing in it saying "do not show me" -- so it cannot disagree with the
// launcher by design. This checks that it does not disagree in practice either,
// against units read off this phone's live cgroups.
//
// It needs the phone's own .desktop files to mean anything, so it is a test you
// run on the surya, not on a build machine. Anywhere without Plasma installed
// every id resolves to Background and the drawer half of the list fails, which
// is a correct answer to a question that was not asked; the exit code says so.
//
//   ./try-appinfo

#include "appinfo.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstdio>

namespace {

// Measured on the surya on 2026-08-28 by walking app.slice and session.slice and
// reading every matching .desktop. Of the 32 live units, Spectacle was the only
// one actually launched from the drawer; Screenglaze and this app are here too
// because they do ship launcher entries even though what burns their CPU is a
// daemon.
const char *DRAWER[] = {
	"org.kde.spectacle",
	"screenglaze",
	"app-usage",
	nullptr,
};

// The three interesting groups, all of which used to be indistinguishable from
// applications: things with NoDisplay=true, @autostart agents, and D-Bus
// activated services with no .desktop at all.
const char *BACKGROUND[] = {
	"plasma-plasmashell",           // the shell -- the biggest consumer here
	"plasma-kwin_wayland",
	"org.kde.discover.notifier",    // NoDisplay=true
	"org.kde.xwaylandvideobridge",  // NoDisplay=true
	"xdg-desktop-portal-gtk",       // NoDisplay=true
	"org.kde.kalendarac",           // @autostart
	"mpris-proxy",                  // @autostart
	"org.kde.modem.daemon",         // @autostart
	"org.bluez.obex",               // dbus-:1.1- activated
	"dbus-broker",
	"vvmd",
	"at-spi-dbus-bus",
	nullptr,
};

int check(const char **ids, AppInfo::Category want, const char *label)
{
	int failed = 0;
	for (int i = 0; ids[i]; ++i) {
		const QString id = QString::fromLatin1(ids[i]);
		const bool ok = AppInfo::category(id) == want;
		std::printf("  %-32s %-10s %s\n", ids[i], label, ok ? "ok" : "FAIL");
		failed += !ok;
	}
	return failed;
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	if (QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
	                           QStringLiteral("org.kde.spectacle.desktop")).isEmpty()) {
		std::fprintf(stderr,
		             "Without this phone's .desktop files the test says nothing.\n"
		             "Run it on the surya.\n");
		return 77;  // the automake convention for "skipped", not "passed"
	}

	int failed = check(DRAWER, AppInfo::Drawer, "drawer");
	failed += check(BACKGROUND, AppInfo::Background, "background");

	std::printf("\n%s (%d failures)\n", failed ? "FAILURES" : "ALL OK", failed);
	return failed ? 1 : 0;
}
