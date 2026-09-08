// SPDX-License-Identifier: LGPL-2.0-or-later
//
// What the per-application screen shows: the store's hourly rows folded into
// one line per consumer, sorted by how much of the battery they took.
//
// The screen is a reader. Everything here comes out of files the daemon wrote;
// nothing in this process touches sysfs, so it opens instantly, works with the
// service stopped by showing the last thing recorded, and costs nothing to
// close. See main.cpp for why the measuring has to be a service.

#pragma once

#include "store.h"

#include <QAbstractListModel>

class UsageModel : public QAbstractListModel {
	Q_OBJECT
	Q_PROPERTY(QString period READ period WRITE setPeriod NOTIFY periodChanged)
	// Background consumers are shown by default. A battery monitor that hides
	// daemons by default hides the answer: on this phone the largest single
	// consumer is the shell itself, which no user ever "opened".
	Q_PROPERTY(bool showBackground READ showBackground WRITE setShowBackground
	           NOTIFY showBackgroundChanged)
	Q_PROPERTY(double measuredJ READ measuredJ NOTIFY reloaded)
	// The deliverable of phase 1: what the model says the applications cost,
	// against what the gauge actually saw leave.
	Q_PROPERTY(double modelledJ READ modelledJ NOTIFY reloaded)
	Q_PROPERTY(QString residual READ residual NOTIFY reloaded)
	Q_PROPERTY(bool empty READ empty NOTIFY reloaded)
	Q_PROPERTY(bool calibrated READ calibrated NOTIFY reloaded)
	Q_PROPERTY(QString note READ note NOTIFY reloaded)
	Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY reloaded)

public:
	enum Role {
		NameRole = Qt::UserRole + 1,
		IconRole,
		ShareRole,     // 0..1 of the measured total, for the bar
		PercentRole,   // the same as a number, for the label
		JoulesRole,
		CpuTextRole,   // "4.7 s", "302 ms" -- already unit-picked
		GpuTextRole,
		KindRole,      // "app" | "screen" | "system"
		BackgroundRole // true when it is not a launcher entry
	};

	explicit UsageModel(QObject *parent = nullptr);

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role) const override;
	QHash<int, QByteArray> roleNames() const override;

	QString period() const { return m_period; }
	void setPeriod(const QString &p);
	bool showBackground() const { return m_showBackground; }
	void setShowBackground(bool show);

	double measuredJ() const { return m_measured / 1000.0; }
	double modelledJ() const { return m_modelled / 1000.0; }
	QString residual() const { return m_residual; }
	bool empty() const { return m_visible.isEmpty(); }
	bool calibrated() const { return m_calibrated; }
	QString note() const { return m_note; }
	int hiddenCount() const { return int(m_rows.size() - m_visible.size()); }

	Q_INVOKABLE void reload();

Q_SIGNALS:
	void periodChanged();
	void showBackgroundChanged();
	void reloaded();

private:
	struct Item {
		QString id;
		QString name;
		QString icon;
		Store::Kind kind = Store::App;
		bool background = true;
		double mj = 0;
		quint64 cpuUsec = 0;
		quint64 gpuNsec = 0;
	};

	Store m_store;
	QString m_period = QStringLiteral("24h");
	bool m_showBackground = true;
	QVector<Item> m_rows;      // everything, in order
	QVector<int> m_visible;    // indices of m_rows that pass the filter
	double m_measured = 0;
	double m_modelled = 0;
	double m_elapsedMs = 0;
	quint64 m_appCpu = 0;
	QString m_residual;
	bool m_calibrated = false;
	QString m_note;

	void applyFilter();
};
