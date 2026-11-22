#pragma once

#include <QString>
#include <QList>

class QAbstractButton;
class QAction;
class QLabel;
class QTableWidget;
class QVBoxLayout;
class QWidget;

namespace ez::ui {

QString stylesheet();
QLabel *hint(const QString &text, QWidget *parent);
void pageHeader(QVBoxLayout *layout, QWidget *page, const QString &title, const QString &description);
void styleTable(QTableWidget *table);
void enableRowMenu(QTableWidget *table, const QList<QAbstractButton *> &buttons,
                   const QList<QAction *> &actions = {});

}
