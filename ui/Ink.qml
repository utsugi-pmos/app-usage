// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Every colour and measurement of the battery screen, in one file.
//
// It is called Ink and NOT Palette, and that is not a style choice: QtQuick
// has had its own `Palette` value type since 6.6, so `import QtQuick` brings
// one into scope and it wins. The symptom is not a name clash error, it is a
// screenful of "Unable to assign [undefined]" because every property was read
// off the wrong type. Screenglaze learned this one first; see its Glaze.qml.
pragma Singleton
import QtQuick

QtObject {
	// The colours follow the user's colour scheme. They were a fixed dark
	// palette, chosen for OLED, and inside System Settings -- light by default
	// -- the page read as a dark slab pasted into a light window; the owner
	// asked for it to follow the theme on 2026-09-17.
	//
	// SystemPalette and not Kirigami.Theme: this is a singleton, and Kirigami's
	// attached Theme needs an Item to inherit from. On Plasma the platform
	// theme fills SystemPalette from the same colour scheme, so both agree.
	readonly property SystemPalette pal: SystemPalette { colorGroup: SystemPalette.Active }

	// A dark scheme is one whose window is darker than its text.
	readonly property bool dark: pal.window.hslLightness < pal.windowText.hslLightness

	function mix(a, b, t) {
		return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
		               a.b + (b.b - a.b) * t, 1)
	}

	readonly property color background: pal.window
	readonly property color surface: pal.base
	// Pressed or selected: the card tinted towards the accent, visible in
	// both a light and a dark scheme.
	readonly property color surfaceDown: mix(pal.base, pal.highlight, dark ? 0.30 : 0.18)

	readonly property color ink: pal.windowText
	readonly property color inkSoft: mix(pal.windowText, pal.window, 0.30)
	readonly property color inkFaint: mix(pal.windowText, pal.window, 0.50)

	// The bars. Applications share the accent colour on purpose: a rainbow
	// implies the colours mean something, and here only the LENGTH means
	// anything. The two non-application consumers get their own so they read
	// as different in kind rather than as just another app.
	readonly property color bar: pal.highlight
	readonly property color barScreen: warning
	readonly property color barSystem: inkFaint

	// Amber reads on a dark background; on a light one the same amber is too
	// pale for text, so it darkens.
	readonly property color warning: dark ? "#f2b544" : "#a86b00"

	readonly property int radius: 14
	readonly property int gap: 12
	// Touch targets. 48 is the smallest thing a thumb hits reliably, and this
	// list is meant to be scrolled with one hand.
	readonly property int row: 64
	readonly property int tap: 48
}
