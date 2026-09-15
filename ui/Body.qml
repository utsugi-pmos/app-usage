// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The battery-per-application screen, as an Item both front-ends embed: the
// standalone window (App.qml) and the System Settings module (main.qml). It
// carries no window and no KCM frame of its own, only the header, the list and
// the calibration footer, so the two front-ends cannot drift apart -- the same
// reason lost-phone keeps its ConfigView in one file.
//
// The three models are passed in rather than read off a context property,
// because a KCM has no context properties of ours: it hands them over as
// `kcm.usage` / `kcm.launcher` / `kcm.calibrator`, and the window binds its own.
//
// The property names deliberately do NOT match the window's context-property
// names (usage/launcher/calibrator). If they did, `Body { usage: usage }` in
// App.qml would resolve the right-hand `usage` to Body's own property -- a
// self-reference that leaves every model undefined and the screen blank.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
	id: bodyRoot

	// UsageModel, Launcher, Calibrator. Set by whoever embeds this.
	property var usageModel
	property var launcherObj
	property var calibratorObj

	ColumnLayout {
		anchors.fill: parent
		spacing: 0

		// --- header: the name, and a jump to Plasma's own battery page -------
		Rectangle {
			Layout.fillWidth: true
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
				// And while it IS opening it says so. Measured, kcmshell6 takes
				// 1.9 s to put a window on screen; without this the button
				// swallows the tap and sits there for two seconds looking broken.
				Rectangle {
					visible: launcherObj ? launcherObj.available : false
					implicitHeight: Ink.tap
					implicitWidth: Math.max(energyLabel.implicitWidth,
					                        openingLabel.implicitWidth + spinner.width + 6)
					                + 2 * Ink.gap
					radius: Ink.radius
					color: (launcherObj && launcherObj.opening) || energyTap.pressed
					       ? Ink.surfaceDown : Ink.surface
					Behavior on color { ColorAnimation { duration: 120 } }

					Label {
						id: energyLabel
						anchors.centerIn: parent
						visible: !(launcherObj && launcherObj.opening)
						text: qsTr("Battery ›")
						color: Ink.inkSoft
						font.pixelSize: 13
					}

					RowLayout {
						anchors.centerIn: parent
						visible: launcherObj ? launcherObj.opening : false
						spacing: 6

						BusyIndicator {
							id: spinner
							running: launcherObj ? launcherObj.opening : false
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
						enabled: !(launcherObj && launcherObj.opening)
						onTapped: launcherObj.openEnergySettings()
					}
				}
			}
		}

		// --- the list --------------------------------------------------------
		AppsPage {
			Layout.fillWidth: true
			Layout.fillHeight: true
			usage: bodyRoot.usageModel
		}

		// --- footer: the honest note, and the way out of it ------------------
		//
		// Saying "not calibrated" and leaving it there put the reader in front
		// of a problem with no handle. The screen half is four minutes and needs
		// nothing but a phone nobody is touching, so the note carries the button
		// that fixes it -- and says why it cannot right now when the charger
		// is in.
		Rectangle {
			Layout.fillWidth: true
			visible: (calibratorObj && (calibratorObj.running || calibratorObj.status !== ""))
			         || (usageModel && (usageModel.note !== "" || usageModel.residual !== ""))
			implicitHeight: visible ? foot.implicitHeight + 2 * Ink.gap : 0
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
					text: calibratorObj && (calibratorObj.running || calibratorObj.status !== "")
					      ? calibratorObj.status : (usageModel ? usageModel.note : "")
					color: Ink.warning
					font.pixelSize: 12
					wrapMode: Text.WordWrap
				}

				// The residual, which is what this whole phase is FOR: how far
				// the model's account of the applications sits from what the
				// gauge saw leave the battery. It decides whether the eBPF phase
				// is worth a kernel rebuild, so it is shown always rather than
				// hidden behind a threshold that would hide the decision.
				Label {
					Layout.fillWidth: true
					visible: usageModel && usageModel.residual !== ""
					         && !(calibratorObj && calibratorObj.running)
					text: usageModel ? usageModel.residual : ""
					color: Ink.inkFaint
					font.pixelSize: 11
					wrapMode: Text.WordWrap
				}

				// The bar is driven by the clock, not by parsing calibrate's
				// output: its progress lines are prose for a person, and turning
				// them into a machine interface would freeze that prose as an API.
				ProgressBar {
					Layout.fillWidth: true
					visible: calibratorObj ? calibratorObj.running : false
					from: 0
					to: 1
					value: calibratorObj ? calibratorObj.progress : 0
				}

				RowLayout {
					Layout.fillWidth: true
					visible: calibratorObj && (!calibratorObj.screenKnown || calibratorObj.running)
					spacing: Ink.gap

					Label {
						Layout.fillWidth: true
						visible: calibratorObj && !calibratorObj.possible && !calibratorObj.running
						         && calibratorObj.blockedReason !== ""
						text: calibratorObj ? calibratorObj.blockedReason : ""
						color: Ink.inkFaint
						font.pixelSize: 11
						wrapMode: Text.WordWrap
					}
					Item {
						Layout.fillWidth: true
						visible: calibratorObj && (calibratorObj.possible || calibratorObj.running)
					}

					Rectangle {
						visible: calibratorObj && (calibratorObj.possible || calibratorObj.running)
						implicitHeight: Ink.tap
						implicitWidth: calLabel.implicitWidth + 2 * Ink.gap
						radius: Ink.radius
						color: calTap.pressed ? Ink.surfaceDown : Ink.background

						Label {
							id: calLabel
							anchors.centerIn: parent
							text: calibratorObj && calibratorObj.running ? qsTr("Cancel")
							                                             : qsTr("Calibrate screen (4 min)")
							color: Ink.ink
							font.pixelSize: 13
						}
						TapHandler {
							id: calTap
							onTapped: calibratorObj.running ? calibratorObj.cancel()
							                                : calibratorObj.start()
						}
					}
				}
			}
		}
	}

	// Once it has finished the list has to be redrawn: the screen row only
	// exists after a calibration, and the daemon has just been restarted.
	Connections {
		target: bodyRoot.calibratorObj
		function onFinished(ok, message) {
			if (ok)
				reloadAfter.start()
		}
	}
	Timer {
		id: reloadAfter
		// Long enough for the restarted collector to take a sweep and flush.
		interval: 5000
		onTriggered: bodyRoot.usageModel.reload()
	}
}
