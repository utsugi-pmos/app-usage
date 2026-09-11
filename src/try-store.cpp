// SPDX-License-Identifier: LGPL-2.0-or-later
//
// The store, and above all its retention.
//
// purge() is the only function in this program that DELETES the user's data, so
// it is the one that has to be exercised rather than reasoned about. It walks a
// directory removing files whose name parses as a date older than a cutoff, and
// the two ways that goes wrong are both silent: keeping everything until the
// disk fills, or eating a file it had no business touching.
//
// Runs anywhere -- it builds its own directory in /tmp and never looks at the
// real store.
//
//   ./try-store

#include "store.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
	std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
	failures += !ok;
}

QString dayFile(const QDir &dir, int daysAgo)
{
	const QString name = QDate::currentDate().addDays(-daysAgo)
	                         .toString(QStringLiteral("yyyy-MM-dd"))
	                     + QStringLiteral(".tsv");
	QFile f(dir.filePath(name));
	if (f.open(QIODevice::WriteOnly))
		f.write("#app-usage\t1\thour\tkind\tid\tmj\tcpu_usec\tgpu_nsec\n");
	return name;
}

void write(const QDir &dir, const QString &name, const char *body)
{
	QFile f(dir.filePath(name));
	if (f.open(QIODevice::WriteOnly))
		f.write(body);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);

	QTemporaryDir tmp;
	if (!tmp.isValid()) {
		std::fprintf(stderr, "could not create a temporary directory\n");
		return 1;
	}
	const QDir dir(tmp.path());

	// --- retention ----------------------------------------------------------
	std::printf("retention (8 days):\n");
	for (int d : {0, 1, 3, 9, 40})
		dayFile(dir, d);
	// Two files it must NOT touch: one that is not a .tsv at all, and one that
	// is a .tsv whose name does not parse as a date. The second is the one that
	// matters -- the guard is QDate::isValid(), and without it a stray file in
	// the state directory would be deleted for having the wrong extension.
	write(dir, QStringLiteral("notes.txt"), "do not delete me");
	write(dir, QStringLiteral("calibration-old.tsv"), "do not delete me");

	Store store(tmp.path());
	const int removed = store.purge(8);

	check(removed == 2, "deletes exactly the two expired files");
	check(dir.exists(QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))
	                 + QStringLiteral(".tsv")),
	      "keeps today");
	check(dir.exists(QDate::currentDate().addDays(-3).toString(QStringLiteral("yyyy-MM-dd"))
	                 + QStringLiteral(".tsv")),
	      "keeps a day from 3 days ago");
	check(!dir.exists(QDate::currentDate().addDays(-9).toString(QStringLiteral("yyyy-MM-dd"))
	                  + QStringLiteral(".tsv")),
	      "deletes a day from 9 days ago");
	check(dir.exists(QStringLiteral("notes.txt")), "does not touch a file that is not .tsv");
	check(dir.exists(QStringLiteral("calibration-old.tsv")),
	      "does not touch a .tsv whose name is not a date");

	// A retention of zero means "keep everything", not "delete everything". The
	// difference is a user's whole history.
	const int none = store.purge(0);
	check(none == 0 && dir.exists(QStringLiteral("notes.txt")),
	      "a retention of 0 deletes nothing");

	// --- round trip ---------------------------------------------------------
	std::printf("\nround trip:\n");
	QTemporaryDir tmp2;
	Store fresh(tmp2.path());

	IntervalResult r;
	r.start = QDateTime::currentDateTimeUtc().addSecs(-30);
	r.end = QDateTime::currentDateTimeUtc();
	r.energyMj = 1234.5;
	r.screenMj = 200.0;
	r.unattributedMj = 34.5;
	r.modelledAppMj = 456.7;
	r.appCpuUsec = 890000;
	r.totalCpuUsec = 2000000;
	r.perApp.insert(QStringLiteral("org.kde.spectacle"), 1000.0);
	r.cpuUsec.insert(QStringLiteral("org.kde.spectacle"), 500000);
	fresh.add(r);
	check(fresh.flush(), "flush to disk");

	// Read it back through a SECOND Store, which is what the report and the
	// screen do: they are separate processes and see only what reached the file.
	Store reader(tmp2.path());
	const QVector<Store::Row> rows = reader.load(r.start.addSecs(-3600));
	double measured = 0, modelled = 0, screen = 0, system = 0, apps = 0;
	for (const Store::Row &row : rows) {
		switch (row.kind) {
		case Store::Measured: measured += row.mj; break;
		case Store::Elapsed: break;
		case Store::Modelled: modelled += row.mj; break;
		case Store::Screen:   screen += row.mj;   break;
		case Store::System:   system += row.mj;   break;
		case Store::App:      apps += row.mj;     break;
		}
	}
	check(qAbs(measured - 1234.5) < 0.1, "the measured energy survives");
	// The one that was silently absent until 2026-08-28: it was computed every
	// interval and never stored, which made the residual -- the deliverable of
	// phase 1 -- impossible to obtain from a day of data.
	check(qAbs(modelled - 456.7) < 0.1, "the MODELLED energy survives");
	check(qAbs(screen - 200.0) < 0.1, "the screen survives");
	check(qAbs(system - 34.5) < 0.1, "the system bucket survives");
	check(qAbs(apps - 1000.0) < 0.1, "the per-application energy survives");

	// --- the midnight rollover ----------------------------------------------
	//
	// The normal case, not an edge one: a daemon that runs overnight holds
	// buckets from two days at once, and flush() rewrites a whole day from
	// memory. If it filed them together, or let one day's rewrite drop the
	// other's rows, somebody loses a day of history and only finds out later.
	std::printf("\nday rollover:\n");
	QTemporaryDir tmp3;
	Store night(tmp3.path());

	const QDateTime beforeMidnight =
	    QDateTime(QDate::currentDate().addDays(-1), QTime(23, 45), QTimeZone::UTC);
	const QDateTime afterMidnight =
	    QDateTime(QDate::currentDate(), QTime(0, 15), QTimeZone::UTC);

	IntervalResult a;
	a.start = beforeMidnight.addSecs(-30);
	a.end = beforeMidnight;
	a.energyMj = 111.0;
	a.perApp.insert(QStringLiteral("yesterday"), 100.0);
	night.add(a);

	IntervalResult b;
	b.start = afterMidnight.addSecs(-30);
	b.end = afterMidnight;
	b.energyMj = 222.0;
	b.perApp.insert(QStringLiteral("today"), 200.0);
	night.add(b);

	check(night.flush(), "flush two days at once");

	const QDir nightDir(tmp3.path());
	const QString fileYesterday = QDate::currentDate().addDays(-1)
	                                  .toString(QStringLiteral("yyyy-MM-dd"))
	                              + QStringLiteral(".tsv");
	const QString fileToday = QDate::currentDate()
	                              .toString(QStringLiteral("yyyy-MM-dd"))
	                          + QStringLiteral(".tsv");
	check(nightDir.exists(fileYesterday), "writes yesterday's file");
	check(nightDir.exists(fileToday), "writes today's file");

	// Each row has to be in ITS day's file, not merged into whichever was
	// written last.
	auto contains = [&nightDir](const QString &file, const char *needle) {
		QFile f(nightDir.filePath(file));
		return f.open(QIODevice::ReadOnly) && f.readAll().contains(needle);
	};
	check(contains(fileYesterday, "yesterday") && !contains(fileYesterday, "\ttoday\t"),
	      "yesterday's file carries only yesterday's rows");
	check(contains(fileToday, "today") && !contains(fileToday, "\tyesterday\t"),
	      "today's file carries only today's rows");

	// And a reader spanning both gets everything: this is what the 7-day period
	// on the screen does.
	Store nightReader(tmp3.path());
	double both = 0;
	for (const Store::Row &row : nightReader.load(beforeMidnight.addSecs(-3600)))
		if (row.kind == Store::Measured)
			both += row.mj;
	check(qAbs(both - 333.0) < 0.1, "reading across midnight sees them both");

	// --- several days -------------------------------------------------------
	//
	// load() walks one file per day between the cutoff and today. With a single
	// day that proves nothing: the screen's 7-day period reads eight files and
	// sums across them, and the lower bound is computed by HOUR, not by day, so
	// an off-by-one there loses or adds a whole day's worth.
	std::printf("\nseveral days:\n");
	QTemporaryDir tmp4;
	Store week(tmp4.path());
	for (int d = 0; d < 9; ++d) {
		IntervalResult w;
		w.end = QDateTime::currentDateTimeUtc().addDays(-d);
		w.start = w.end.addSecs(-30);
		w.energyMj = 100000.0;
		w.modelledAppMj = 40000.0;
		w.appCpuUsec = 4000000;
		w.perApp.insert(QStringLiteral("plasma-plasmashell"), 80000.0);
		week.add(w);
	}
	check(week.flush(), "flush nine days");
	check(QDir(tmp4.path()).entryList(QStringList() << QStringLiteral("*.tsv"),
	                                  QDir::Files).size() == 9,
	      "one file per day");

	auto sumMeasured = [](const Store &s, const QDateTime &since) {
		double total = 0;
		for (const Store::Row &row : s.load(since))
			if (row.kind == Store::Measured)
				total += row.mj;
		return total;
	};
	Store weekReader(tmp4.path());
	const QDateTime now = QDateTime::currentDateTimeUtc();
	check(qAbs(sumMeasured(weekReader, now.addDays(-7)) - 800000.0) < 1.0,
	      "7 days sum eight days, not nine");
	check(qAbs(sumMeasured(weekReader, now.addSecs(-24 * 3600)) - 200000.0) < 1.0,
	      "24 h sum two days");

	std::printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL OK", failures);
	return failures ? 1 : 0;
}
