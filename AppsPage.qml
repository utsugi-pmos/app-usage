// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The list Plasma Mobile does not have: what took the battery, in order.
//
// Background consumers are shown by DEFAULT and merely marked, not hidden. On
// this phone the largest single consumer is the shell itself, which nobody ever
// "opened"; a battery monitor whose default view hides that is hiding the
// answer. The filter is one tap away for when you want only the apps.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import AppUsage

Item {
	id: page

	ColumnLayout {
		anchors.fill: parent
		spacing: 0

		// The period selector. Three buttons rather than a combo box: on a phone
		// a combo needs two taps and a popup to change something you will change
		// constantly while looking at this screen.
		RowLayout {
			Layout.fillWidth: true
			Layout.margins: Tinta.gap
			spacing: 8

			Repeater {
				model: [
					{ key: "1h",  label: qsTr("1 hour") },
					{ key: "24h", label: qsTr("24 hours") },
					{ key: "7d",  label: qsTr("7 days") },
				]
				delegate: Rectangle {
					required property var modelData
					Layout.fillWidth: true
					implicitHeight: Tinta.tap
					radius: Tinta.radius
					color: usage.period === modelData.key ? Tinta.surfaceDown : Tinta.surface

					Label {
						anchors.centerIn: parent
						text: modelData.label
						color: usage.period === modelData.key ? Tinta.ink : Tinta.inkSoft
						font.pixelSize: 14
					}
					TapHandler { onTapped: usage.period = modelData.key }
				}
			}
		}

		RowLayout {
			Layout.fillWidth: true
			Layout.leftMargin: Tinta.gap
			Layout.rightMargin: Tinta.gap
			Layout.bottomMargin: Tinta.gap
			spacing: Tinta.gap

			Label {
				Layout.fillWidth: true
				// Decimals below 10 J. Rounding to whole joules turned 0.06 J
				// into "0 J measured", which reads as "nothing was measured"
				// while a list of applications sits underneath it.
				text: usage.measuredJ >= 1000
				      ? qsTr("%1 kJ measured").arg((usage.measuredJ / 1000).toFixed(1))
				      : qsTr("%1 J measured").arg(
				            usage.measuredJ.toFixed(usage.measuredJ < 10 ? 2 : 0))
				color: Tinta.inkSoft
				font.pixelSize: 13
			}

			Rectangle {
				implicitHeight: 34
				implicitWidth: filterLabel.implicitWidth + 2 * Tinta.gap
				radius: Tinta.radius
				color: usage.showBackground ? Tinta.surfaceDown : Tinta.surface
				Label {
					id: filterLabel
					anchors.centerIn: parent
					text: usage.showBackground ? qsTr("All") : qsTr("Apps only")
					color: Tinta.inkSoft
					font.pixelSize: 12
				}
				TapHandler { onTapped: usage.showBackground = !usage.showBackground }
			}
		}

		ListView {
			id: list
			Layout.fillWidth: true
			Layout.fillHeight: true
			Layout.leftMargin: Tinta.gap
			Layout.rightMargin: Tinta.gap
			spacing: 6
			clip: true
			model: usage

			delegate: Item {
				id: entry
				required property string name
				required property string kind
				required property bool background
				required property real share
				required property real percent
				required property real joules
				required property string cpuText
				required property string gpuText

				width: ListView.view.width
				height: card.height

				readonly property color barColor: kind === "screen" ? Tinta.barScreen
				                                : kind === "system" ? Tinta.barSystem
				                                : Tinta.bar
				property bool expanded: false

				Rectangle {
					id: card
					width: parent.width
					height: content.implicitHeight + 2 * Tinta.gap
					radius: Tinta.radius
					color: Tinta.surface

					ColumnLayout {
						id: content
						anchors.fill: parent
						anchors.margins: Tinta.gap
						spacing: 8

						RowLayout {
							Layout.fillWidth: true
							spacing: 8

							Label {
								text: entry.name
								// Dimmed rather than hidden: a background daemon
								// eating the battery still has to be findable.
								color: entry.background ? Tinta.inkSoft : Tinta.ink
								font.pixelSize: 15
								elide: Text.ElideRight
								Layout.fillWidth: true
							}
							// Faint text rather than a filled chip. On this phone
							// almost every row is a background service, and a pill
							// on each of them turns the column into a wall of
							// badges that competes with the numbers -- which are
							// the reason anyone opened this.
							Label {
								visible: entry.background && entry.kind === "app"
								text: qsTr("background")
								color: Tinta.inkFaint
								font.pixelSize: 10
							}
							Label {
								// Below a tenth of a percent the number stops being
								// information and starts being noise, so it says so
								// instead of printing a column of 0.0 %.
								text: entry.percent >= 0.1
								      ? qsTr("%1 %").arg(entry.percent.toFixed(1))
								      : qsTr("< 0.1 %")
								color: Tinta.inkSoft
								font.pixelSize: 14
							}
						}

						// The bar. The only thing on the row comparable at a
						// glance, so it gets the width.
						Rectangle {
							Layout.fillWidth: true
							height: 6
							radius: 3
							color: Tinta.surfaceDown

							Rectangle {
								width: Math.max(parent.width * entry.share, entry.share > 0 ? 3 : 0)
								height: parent.height
								radius: parent.radius
								color: entry.barColor
								opacity: entry.background ? 0.55 : 1.0
								Behavior on width { NumberAnimation { duration: 160 } }
							}
						}

						// The breakdown, on tap. Folded away by default because it
						// answers a second question, and the first one is "which
						// app", not "how much CPU".
						ColumnLayout {
							Layout.fillWidth: true
							visible: entry.expanded
							spacing: 2

							Label {
								text: qsTr("%1 J").arg(entry.joules.toFixed(2))
								color: Tinta.inkFaint
								font.pixelSize: 12
							}
							Label {
								visible: entry.cpuText !== ""
								text: qsTr("CPU: %1").arg(entry.cpuText)
								color: Tinta.inkFaint
								font.pixelSize: 12
							}
							Label {
								visible: entry.gpuText !== ""
								text: qsTr("GPU: %1").arg(entry.gpuText)
								color: Tinta.inkFaint
								font.pixelSize: 12
							}
						}
					}

					TapHandler { onTapped: entry.expanded = !entry.expanded }
				}
			}

			// The data only changes when the daemon flushes, every five minutes.
			// A timer that redraws on its own is less to explain than a
			// pull-to-refresh gesture that usually does nothing.
			Timer {
				interval: 60000
				running: page.visible
				repeat: true
				onTriggered: usage.reload()
			}

			// Two different empties, and only one of them is "nothing happened".
			// With the filter on, a phone where nothing user-launched ran shows
			// an empty list while the store is full -- saying "nothing to show
			// yet" there would be a plain falsehood sitting over real data.
			Label {
				anchors.centerIn: parent
				width: parent.width - 4 * Tinta.gap
				visible: usage.empty
				text: usage.hiddenCount > 0
				      // With '+', not by juxtaposition: in QML two adjacent
				      // literals do NOT concatenate as in C, it is a syntax
				      // error, and lupdate simply does not see the string.
				      ? qsTr("No launcher applications in this period.")
				        + "\n" + qsTr("Everything that ran was a background service.")
				      : qsTr("Nothing to show yet")
				horizontalAlignment: Text.AlignHCenter
				wrapMode: Text.WordWrap
				color: Tinta.inkFaint
				font.pixelSize: 16
			}
		}

		Label {
			Layout.fillWidth: true
			Layout.margins: Tinta.gap
			visible: !usage.showBackground && usage.hiddenCount > 0
			text: qsTr("%n background item(s) hidden", "", usage.hiddenCount)
			color: Tinta.inkFaint
			font.pixelSize: 11
		}
	}
}
