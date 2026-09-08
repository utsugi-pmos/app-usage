// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Every colour and measurement of the battery screen, in one file.
//
// It is called Tinta and NOT Palette, and that is not a style choice: QtQuick
// has had its own `Palette` value type since 6.6, so `import QtQuick` brings
// one into scope and it wins. The symptom is not a name clash error, it is a
// screenful of "Unable to assign [undefined]" because every property was read
// off the wrong type. Screenglaze learned this one first; see its Glaze.qml.
pragma Singleton
import QtQuick

QtObject {
	// Dark, because this is a phone screen you open to find out why the battery
	// is going and the panel itself is on the list of suspects. On an OLED a
	// near-black background is the cheapest row in the table.
	readonly property color background: "#17181c"
	readonly property color surface: "#212329"
	readonly property color surfaceDown: "#2e313a"

	readonly property color ink: "#ffffff"
	readonly property color inkSoft: "#a9aeba"
	readonly property color inkFaint: "#70757f"

	// The bars. Applications share one colour on purpose: a rainbow implies the
	// colours mean something, and here only the LENGTH means anything.
	readonly property color bar: "#5b8def"
	// The two non-application consumers get their own so they read as different
	// in kind rather than as just another app.
	readonly property color barScreen: "#f2b544"
	readonly property color barSystem: "#6f7480"

	readonly property color warning: "#f2b544"

	// The charge bar, and the only place in the app where colour carries meaning
	// rather than just separating things. Read at arm's length: fine, getting
	// low, or a problem. Charging gets its own colour instead of green so that
	// "plugged in at 20 %" does not look like "20 % and falling".
	readonly property color good: "#4caf72"
	readonly property color low: "#f2b544"
	readonly property color critical: "#e2574c"
	readonly property color charging: "#5b8def"

	readonly property int radius: 14
	readonly property int gap: 12
	// Touch targets. 48 is the smallest thing a thumb hits reliably, and this
	// list is meant to be scrolled with one hand.
	readonly property int row: 64
	readonly property int tap: 48
}
