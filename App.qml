// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The standalone window. All the screen is in Body.qml, shared with the System
// Settings module (ui/main.qml), so the two front-ends cannot drift apart.
//
// There is deliberately no battery-status page here. The phone already has one
// -- Plasma's Energy settings -- and it is a better one than a copy would be,
// because it is the one that gets updated with Plasma. So the header links to
// the real page instead.
//
// This is a reader. Every number came out of files the daemon wrote, so it
// opens instantly, works with the service stopped by showing the last thing
// recorded, and costs nothing to close. A battery screen that drains the
// battery is a joke that writes itself.
import QtQuick
import QtQuick.Controls

ApplicationWindow {
	id: root

	// Sized for the surya's panel. On a desktop it opens as a tall narrow
	// window, which is the right shape for this list anyway.
	width: 400
	height: 800
	visible: true
	// Not qsTr(): "Battery usage" is the program's name, not a label.
	// A launcher entry whose name changes with the locale is one you
	// cannot tell anyone to look for.
	title: "Battery usage"
	color: Ink.background

	// usage, launcher, calibrator are context properties set in ui.cpp. Body's
	// properties are deliberately named differently so these bindings resolve to
	// the context properties, not to Body's own (which would self-reference).
	Body {
		anchors.fill: parent
		usageModel: usage
		launcherObj: launcher
		calibratorObj: calibrator
	}
}
