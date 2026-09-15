// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Formatting that both pages need, in one place so the two cannot drift into
// saying "1.5 h" and "1 h 30 min" about the same number.
pragma Singleton
import QtQuick

QtObject {
	// Seconds to something a person reads at a glance. Deliberately coarse:
	// "2 h 10 min until full" is useful, "2 h 9 min 47 s" is a stopwatch, and
	// the underlying estimate is nowhere near that good anyway.
	function duration(seconds) {
		if (seconds <= 0)
			return ""
		var m = Math.round(seconds / 60)
		if (m < 60)
			return qsTr("%1 min").arg(m)
		var h = Math.floor(m / 60)
		var rest = m % 60
		if (h >= 24) {
			var d = Math.floor(h / 24)
			return qsTr("%1 d %2 h").arg(d).arg(h % 24)
		}
		return rest === 0 ? qsTr("%1 h").arg(h) : qsTr("%1 h %2 min").arg(h).arg(rest)
	}
}
