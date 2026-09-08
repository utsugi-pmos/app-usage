// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Where the intervals go once they are attributed.
//
// The collector produces one IntervalResult every 30 s. Keeping all of them
// would be 2880 records a day per application, so they are folded into hourly
// buckets on the way in, which is the resolution the screen actually shows: a
// list of applications over "since the last charge", "24 h" or "7 days".
//
// The format is a tab-separated text file per day, not a database. That is a
// deliberate choice for this phone: when something looks wrong the first thing
// anyone does is ssh in and read the file, and a plain file answers that in one
// `cat` where SQLite needs a client that may not be installed. The volume makes
// it easy -- 7 days at 24 hours with ~50 applications is a few thousand short
// lines, and the whole retention window fits comfortably in memory.

#pragma once

#include "collector.h"

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>
#include <QVector>

class Store {
public:
	// What a row is about. Applications share the file with the three
	// non-application consumers, because a reader that wants "where did the
	// battery go" needs all four to add up to the measured total.
	enum Kind {
		App,       // one application, id is its appId
		Screen,    // the panel, charged as its own consumer the way Android does
		System,    // the honest residual: modem, radios, kernel threads, idle floor
		Measured,  // what the gauge said left the battery, before any modelling
		// What the energy model says the applications cost, before any capping.
		//
		// This is THE deliverable of phase 1 and it used to be computed every
		// interval and thrown away, which made the one number the phase exists
		// to produce impossible to obtain: comparing it against Measured across
		// a real day is what decides whether phase 2 -- eBPF over sched_switch
		// -- is worth a kernel rebuild.
		Modelled,
		// Milliseconds REALLY measured within the hour, in the mj field.
		//
		// Without this an hour with five minutes of sampling looks the same as
		// one with sixty, and the average power -- which is how you check whether
		// everything else makes sense -- cannot be computed. The daemon only
		// measures while unplugged, so the gap between one and the other is the
		// norm, not the exception.
		Elapsed,
	};

	explicit Store(const QString &dir);

	const QString &dir() const { return m_dir; }
	QString errorString() const { return m_error; }

	// Folds one interval into its hour bucket. Cheap and in memory; nothing
	// touches the disk until flush().
	void add(const IntervalResult &r);

	// Writes every bucket that has changed since the last call. Called both when
	// an hour rolls over and periodically, so that a phone that loses power --
	// which this one is documented to do under load, see BOOT-03 -- loses
	// minutes rather than a day.
	bool flush();

	// Drops day files older than keepDays. Runs at startup and after each hour
	// rollover; retention is in days because the longest period the screen
	// offers is 7.
	int purge(int keepDays);

	struct Row {
		QDateTime hour;   // start of the hour bucket, UTC
		Kind kind = App;
		QString id;       // appId for App, empty otherwise
		double mj = 0;
		quint64 cpuUsec = 0;
		quint64 gpuNsec = 0;
	};

	// Everything recorded at or after `since`, including buckets still in memory
	// that have not been flushed. Sorted by hour.
	QVector<Row> load(const QDateTime &since) const;

	static QString kindName(Kind k);
	static bool kindFromName(const QString &name, Kind *out);

private:
	QString m_dir;
	QString m_error;

	// hour -> (kind, id) -> row. QMap rather than QHash for the outer level so
	// that flushing walks the hours in order and a day file stays sorted.
	QMap<QDateTime, QHash<QString, Row>> m_buckets;
	QSet<QDateTime> m_dirtyDays;

	QString pathForDay(const QDate &day) const;
	static QString bucketKey(Kind kind, const QString &id);
	bool writeDay(const QDate &day);
	QVector<Row> readDay(const QDate &day) const;
};
