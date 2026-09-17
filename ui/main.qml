// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The System Settings module front-end. The whole screen is Body.qml, shared
// with the standalone window; this only wraps it in the KCM frame and hands it
// the three models the C++ side exposes as kcm.usage / kcm.launcher /
// kcm.calibrator.
//
// AbstractKCM rather than SimpleKCM on purpose: the list inside Body scrolls
// itself (its ListView fills the height), and SimpleKCM's own ScrollView would
// nest a scroll inside a scroll. There is no Apply button because there is
// nothing to apply -- this is a reader, not a settings form.
import QtQuick

import org.kde.kcmutils as KCM
import org.kde.kirigami as Kirigami

KCM.AbstractKCM {
	id: root

	// The list draws its own background (Ink.background); keep the frame from
	// adding padding around it so the screen looks the same as the window.
	leftPadding: 0
	rightPadding: 0
	topPadding: 0
	bottomPadding: 0

	// The link to Plasma's own battery page, where a module keeps its links: in
	// the header, next to the name System Settings already draws.
	actions: [
		Kirigami.Action {
			text: kcm.launcher.opening ? qsTr("Opening…") : qsTr("Battery")
			icon.name: "battery-symbolic"
			visible: kcm.launcher.available
			enabled: !kcm.launcher.opening
			onTriggered: kcm.launcher.openEnergySettings()
		}
	]

	Body {
		anchors.fill: parent
		showHeader: false
		usageModel: kcm.usage
		launcherObj: kcm.launcher
		calibratorObj: kcm.calibrator
	}
}
