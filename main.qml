// SPDX-License-Identifier: LGPL-2.0-or-later
//
// One screen: where the battery went, per application.
//
// There is deliberately no battery-status page here. The phone already has one
// -- Plasma's Energy settings -- and it is a better one than a copy would be,
// because it is the one that gets updated with Plasma. Building a second would
// mean either duplicating it and letting the two drift, or embedding
// org.kde.kcm.power.mobile.private, a module whose name says out loud that it
// is not something to build on. So the header links to the real page instead.
//
// This is a reader. Every number came out of files the daemon wrote, so it
// opens instantly, works with the service stopped by showing the last thing
// recorded, and costs nothing to close. A battery screen that drains the
// battery is a joke that writes itself.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AppUsage

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

	header: Rectangle {
		implicitHeight: headerRow.implicitHeight + 2 * Ink.gap
		color: Ink.background

		RowLayout {
			id: headerRow
			x: Ink.gap
			y: Ink.gap
			width: parent.width - 2 * Ink.gap
			spacing: Ink.gap

			ColumnLayout {
				Layout.fillWidth: true
				spacing: 1
				Label {
					text: "Battery usage"
					color: Ink.ink
					font.pixelSize: 20
					font.bold: true
				}
				Label {
					text: qsTr("by application")
					color: Ink.inkFaint
					font.pixelSize: 12
				}
			}

			// Hidden rather than dead when there is nothing to open: a button
			// that does nothing when pressed is worse than an absent one.
			//
			// And while it IS opening it says so. Measured, kcmshell6 takes 1.9 s
			// to put a window on screen; without this the button swallows the tap
			// and sits there for two seconds looking broken, which is how an app
			// teaches you not to trust it.
			Rectangle {
				visible: launcher.available
				implicitHeight: Ink.tap
				// Width follows the wider of the two labels so the header does
				// not jump when the text changes.
				implicitWidth: Math.max(energyLabel.implicitWidth,
				                        openingLabel.implicitWidth + spinner.width + 6)
				                + 2 * Ink.gap
				radius: Ink.radius
				color: launcher.opening || energyTap.pressed ? Ink.surfaceDown
				                                             : Ink.surface
				Behavior on color { ColorAnimation { duration: 120 } }

				Label {
					id: energyLabel
					anchors.centerIn: parent
					visible: !launcher.opening
					text: qsTr("Battery ›")
					color: Ink.inkSoft
					font.pixelSize: 13
				}

				RowLayout {
					anchors.centerIn: parent
					visible: launcher.opening
					spacing: 6

					BusyIndicator {
						id: spinner
						running: launcher.opening
						implicitWidth: 16
						implicitHeight: 16
					}
					Label {
						id: openingLabel
						text: qsTr("Opening…")
						color: Ink.inkSoft
						font.pixelSize: 13
					}
				}

				TapHandler {
					id: energyTap
					// The C++ side refuses a second launch while one is in
					// flight, so a double tap cannot open two windows.
					enabled: !launcher.opening
					onTapped: launcher.openEnergySettings()
				}
			}
		}
	}

	AppsPage {
		anchors.fill: parent
	}

	// The honest note, and the way out of it.
	//
	// Saying "not calibrated" and leaving it there put the reader in front of a
	// problem with no handle. The screen half is four minutes and needs nothing
	// but a phone nobody is touching, so the note carries the button that fixes
	// it -- and says why it cannot right now when the charger is in.
	footer: Rectangle {
		visible: calibrator.running || usage.note !== "" || usage.residual !== ""
		         || calibrator.status !== ""
		height: visible ? foot.implicitHeight + 2 * Ink.gap : 0
		color: Ink.surface

		ColumnLayout {
			id: foot
			x: Ink.gap
			y: Ink.gap
			width: parent.width - 2 * Ink.gap
			spacing: 8

			Label {
				Layout.fillWidth: true
				visible: text !== ""
				text: calibrator.running || calibrator.status !== "" ? calibrator.status
				                                                     : usage.note
				color: Ink.warning
				font.pixelSize: 12
				wrapMode: Text.WordWrap
			}

			// The residual, which is what this whole phase is FOR: how far the
			// model's account of the applications sits from what the gauge saw
			// leave the battery. It decides whether the eBPF phase is worth a
			// kernel rebuild, so it is shown always rather than hidden behind a
			// threshold that would hide the decision.
			Label {
				Layout.fillWidth: true
				visible: usage.residual !== "" && !calibrator.running
				text: usage.residual
				color: Ink.inkFaint
				font.pixelSize: 11
				wrapMode: Text.WordWrap
			}

			// The bar is driven by the clock, not by parsing calibrate's output:
			// its progress lines are prose for a person, and turning them into a
			// machine interface would freeze that prose as an API.
			ProgressBar {
				Layout.fillWidth: true
				visible: calibrator.running
				from: 0
				to: 1
				value: calibrator.progress
			}

			RowLayout {
				Layout.fillWidth: true
				visible: !calibrator.screenKnown || calibrator.running
				spacing: Ink.gap

				Label {
					Layout.fillWidth: true
					visible: !calibrator.possible && !calibrator.running
					         && calibrator.blockedReason !== ""
					text: calibrator.blockedReason
					color: Ink.inkFaint
					font.pixelSize: 11
					wrapMode: Text.WordWrap
				}
				Item {
					Layout.fillWidth: true
					visible: calibrator.possible || calibrator.running
				}

				Rectangle {
					visible: calibrator.possible || calibrator.running
					implicitHeight: Ink.tap
					implicitWidth: calLabel.implicitWidth + 2 * Ink.gap
					radius: Ink.radius
					color: calTap.pressed ? Ink.surfaceDown : Ink.background

					Label {
						id: calLabel
						anchors.centerIn: parent
						text: calibrator.running ? qsTr("Cancel")
						                         : qsTr("Calibrate screen (4 min)")
						color: Ink.ink
						font.pixelSize: 13
					}
					TapHandler {
						id: calTap
						onTapped: calibrator.running ? calibrator.cancel() : calibrator.start()
					}
				}
			}
		}
	}

	// Once it has finished the list has to be redrawn: the screen row only
	// exists after a calibration, and the daemon has just been restarted.
	Connections {
		target: calibrator
		function onFinished(ok, message) {
			if (ok)
				reloadAfter.start()
		}
	}
	Timer {
		id: reloadAfter
		// Long enough for the restarted collector to take a sweep and flush.
		interval: 5000
		onTriggered: usage.reload()
	}
}
