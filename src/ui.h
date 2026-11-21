#pragma once

#include <QString>

class QLabel;
class QTableWidget;
class QVBoxLayout;
class QWidget;

namespace ez::ui {

QString stylesheet();
QLabel *hint(const QString &text, QWidget *parent);
void pageHeader(QVBoxLayout *layout, QWidget *page, const QString &title, const QString &description);
void styleTable(QTableWidget *table);

}
