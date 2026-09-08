// SPDX-License-Identifier: LGPL-2.0-or-later

#include "store.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>

namespace {

// Bumped when the column layout changes. A file whose header does not match is
// skipped rather than parsed with the wrong columns, because half-understood
// numbers are worse than missing ones on a screen whose whole job is to be
// believed.
const char *HEADER = "#app-usage\t1\thour\tkind\tid\tmj\tcpu_usec\tgpu_nsec";

QDateTime hourOf(const QDateTime &t)
{
	QDateTime h = t.toUTC();
	QTime clock = h.time();
	h.setTime(QTime(clock.hour(), 0, 0));
	return h;
}

} // namespace

Store::Store(const QString &dir)
    : m_dir(dir)
{
	QDir().mkpath(m_dir);

	// Today's rows are read back into the buckets at startup, and that is not an
	// optimisation -- it is what stops a restart from eating the current hour.
	//
	// flush() rewrites a day from memory and treats memory as authoritative for
	// every hour it holds, which is right while the process runs. Across a
	// restart it stops being right: the fresh buckets hold only what happened
	// since boot, so the hour in progress would be rewritten without the part
	// that came before. Measured while testing this: 22 rows became 14 and
	// 20.6 J of accounted energy vanished, on a service that had been up for
	// ninety seconds. On a phone the service restarts on upgrade, on logout and
	// on the power cuts this one is documented to have (BOOT-03), so that
	// would not have been rare.
	const QVector<Row> today = readDay(QDateTime::currentDateTimeUtc().date());
	for (const Row &row : today)
		m_buckets[row.hour].insert(bucketKey(row.kind, row.id), row);
}

QString Store::kindName(Kind k)
{
	switch (k) {
	case App:      return QStringLiteral("app");
	case Screen:   return QStringLiteral("screen");
	case System:   return QStringLiteral("system");
	case Measured: return QStringLiteral("measured");
	case Modelled: return QStringLiteral("modelled");
	case Elapsed:  return QStringLiteral("elapsed");
	}
	return QStringLiteral("app");
}

bool Store::kindFromName(const QString &name, Kind *out)
{
	if (name == QLatin1String("app"))           *out = App;
	else if (name == QLatin1String("screen"))   *out = Screen;
	else if (name == QLatin1String("system"))   *out = System;
	else if (name == QLatin1String("measured")) *out = Measured;
	else if (name == QLatin1String("modelled")) *out = Modelled;
	else if (name == QLatin1String("elapsed"))  *out = Elapsed;
	else return false;
	return true;
}

QString Store::bucketKey(Kind kind, const QString &id)
{
	return kindName(kind) + QLatin1Char('\t') + id;
}

QString Store::pathForDay(const QDate &day) const
{
	return m_dir + QLatin1Char('/') + day.toString(QStringLiteral("yyyy-MM-dd"))
	       + QStringLiteral(".tsv");
}

void Store::add(const IntervalResult &r)
{
	// The interval is filed under the hour its measurement ENDED in. An interval
	// that straddles an hour boundary is 30 s at worst, so splitting it would buy
	// a fraction of a percent of one bucket at the cost of code that has to
	// apportion every field. Not worth it.
	const QDateTime hour = hourOf(r.end);
	QHash<QString, Row> &bucket = m_buckets[hour];

	auto fold = [&](Kind kind, const QString &id, double mj,
	                quint64 cpuUsec, quint64 gpuNsec) {
		if (mj <= 0 && cpuUsec == 0 && gpuNsec == 0)
			return;
		Row &row = bucket[bucketKey(kind, id)];
		row.hour = hour;
		row.kind = kind;
		row.id = id;
		row.mj += mj;
		row.cpuUsec += cpuUsec;
		row.gpuNsec += gpuNsec;
	};

	for (auto it = r.perApp.constBegin(); it != r.perApp.constEnd(); ++it)
		fold(App, it.key(), it.value(), r.cpuUsec.value(it.key()),
		     r.gpuNsec.value(it.key()));

	fold(Screen, QString(), r.screenMj, 0, 0);
	fold(System, QString(), r.unattributedMj, 0, 0);
	// The measured total is stored alongside the split rather than left to be
	// recomputed by summing the others. They should add up, and keeping the
	// original means a reader can check that they do instead of assuming it.
	fold(Measured, QString(), r.energyMj, r.totalCpuUsec, 0);
	// Stored beside the measurement rather than derived later: it depends on the
	// frequency mix of THAT interval, which nothing downstream can reconstruct.
	fold(Modelled, QString(), r.modelledAppMj, r.appCpuUsec, 0);
	// Adding a new kind is backward compatible: kindFromName rejects what it
	// does not know and the row is skipped, so older files keep being read and
	// today's files are read by an old version without breaking.
	fold(Elapsed, QString(), double(r.start.msecsTo(r.end)), 0, 0);

	m_dirtyDays.insert(QDateTime(hour.date(), QTime(0, 0), QTimeZone::UTC));
}

bool Store::flush()
{
	if (m_dirtyDays.isEmpty())
		return true;
	bool ok = true;
	const QSet<QDateTime> days = m_dirtyDays;
	for (const QDateTime &day : days) {
		if (writeDay(day.date()))
			m_dirtyDays.remove(day);
		else
			ok = false;
	}
	return ok;
}

bool Store::writeDay(const QDate &day)
{
	// The whole day is rewritten from the in-memory buckets rather than appended
	// to, because a bucket keeps accumulating for the hour it is in: appending
	// would leave several partial rows for the same hour and make every reader
	// responsible for summing them. A day file is a few thousand short lines, so
	// rewriting costs nothing.
	//
	// Hours already dropped from memory are read back and preserved, which is
	// what keeps a restart from truncating the earlier part of today.
	QMap<QDateTime, QHash<QString, Row>> merged;
	const QVector<Row> onDisk = readDay(day);
	for (const Row &row : onDisk) {
		if (m_buckets.contains(row.hour))
			continue;  // memory is authoritative for hours it still holds
		merged[row.hour].insert(bucketKey(row.kind, row.id), row);
	}
	for (auto it = m_buckets.constBegin(); it != m_buckets.constEnd(); ++it) {
		if (it.key().date() == day)
			merged.insert(it.key(), it.value());
	}

	// QSaveFile so a phone that loses power mid-write keeps yesterday's file
	// instead of a truncated one.
	QSaveFile f(pathForDay(day));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		m_error = QStringLiteral("cannot write %1: %2")
		          .arg(f.fileName(), f.errorString());
		return false;
	}

	QByteArray out;
	out += HEADER;
	out += '\n';
	for (auto hourIt = merged.constBegin(); hourIt != merged.constEnd(); ++hourIt) {
		const QByteArray stamp = hourIt.key().toString(Qt::ISODate).toUtf8();
		for (const Row &row : hourIt.value()) {
			out += stamp;
			out += '\t';
			out += kindName(row.kind).toUtf8();
			out += '\t';
			out += row.id.toUtf8();
			out += '\t';
			out += QByteArray::number(row.mj, 'f', 1);
			out += '\t';
			out += QByteArray::number(qulonglong(row.cpuUsec));
			out += '\t';
			out += QByteArray::number(qulonglong(row.gpuNsec));
			out += '\n';
		}
	}
	f.write(out);
	if (!f.commit()) {
		m_error = QStringLiteral("cannot close %1: %2")
		          .arg(f.fileName(), f.errorString());
		return false;
	}
	return true;
}

QVector<Store::Row> Store::readDay(const QDate &day) const
{
	QVector<Row> rows;
	QFile f(pathForDay(day));
	// Not QTextStream, and not by accident: see the comment on readFile() in
	// collector.cpp. These files are on real storage rather than kernfs so the
	// size is honest, but keeping one way of reading a file means nobody has to
	// remember which one is safe where.
	if (!f.open(QIODevice::ReadOnly))
		return rows;
	const QList<QByteArray> lines = f.readAll().split('\n');
	if (lines.isEmpty() || !lines.first().startsWith("#app-usage\t1\t"))
		return rows;  // absent, empty, or written by a version with other columns

	for (const QByteArray &line : lines) {
		if (line.isEmpty() || line.startsWith('#'))
			continue;
		const QList<QByteArray> f6 = line.split('\t');
		if (f6.size() != 6)
			continue;
		Row row;
		row.hour = QDateTime::fromString(QString::fromUtf8(f6[0]), Qt::ISODate).toUTC();
		if (!row.hour.isValid() || !kindFromName(QString::fromUtf8(f6[1]), &row.kind))
			continue;
		row.id = QString::fromUtf8(f6[2]);
		row.mj = f6[3].toDouble();
		row.cpuUsec = f6[4].toULongLong();
		row.gpuNsec = f6[5].toULongLong();
		rows.append(row);
	}
	return rows;
}

QVector<Store::Row> Store::load(const QDateTime &since) const
{
	QVector<Row> rows;
	const QDate from = since.toUTC().date();
	const QDate to = QDateTime::currentDateTimeUtc().date();

	QSet<QDateTime> inMemory;
	for (auto it = m_buckets.constBegin(); it != m_buckets.constEnd(); ++it)
		inMemory.insert(it.key());

	for (QDate d = from; d <= to; d = d.addDays(1)) {
		const QVector<Row> day = readDay(d);
		for (const Row &row : day) {
			// An hour still held in memory has not necessarily been flushed, so
			// the disk copy of it may be stale. Memory wins.
			if (inMemory.contains(row.hour) || row.hour < hourOf(since))
				continue;
			rows.append(row);
		}
	}
	for (auto it = m_buckets.constBegin(); it != m_buckets.constEnd(); ++it) {
		if (it.key() < hourOf(since))
			continue;
		for (const Row &row : it.value())
			rows.append(row);
	}

	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
		return a.hour < b.hour;
	});
	return rows;
}

int Store::purge(int keepDays)
{
	if (keepDays <= 0)
		return 0;
	const QDate cutoff = QDateTime::currentDateTimeUtc().date().addDays(-keepDays);
	int removed = 0;
	QDir d(m_dir);
	const QStringList files = d.entryList(QStringList() << QStringLiteral("*.tsv"),
	                                      QDir::Files);
	for (const QString &name : files) {
		const QDate day = QDate::fromString(name.left(10), QStringLiteral("yyyy-MM-dd"));
		if (day.isValid() && day < cutoff && d.remove(name))
			++removed;
	}
	return removed;
}
