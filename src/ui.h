#pragma once

#include <QString>
#include <QList>
#include <functional>

class QAbstractButton;
class QAction;
class QComboBox;
class QLabel;
class QTableWidget;
class QVBoxLayout;
class QWidget;

namespace ez::ui {

enum class Theme { Dark, Light };

void applyTheme(Theme theme);
QLabel *hint(const QString &text, QWidget *parent);
void pageHeader(QVBoxLayout *layout, QWidget *page, const QString &title, const QString &description = {});
void fitComboBox(QComboBox *box, int minimumCharacters);
void styleTable(QTableWidget *table);
void styleTaskStatus(QTableWidget *table, int column);
int selectedRow(const QTableWidget *table);
void enableRowMenu(QTableWidget *table, const QList<QAbstractButton *> &buttons,
                   const QList<QAction *> &actions = {});
QWidget *engineCompatibilityActions(QWidget *parent, const std::function<void()> &details,
                                    const std::function<void()> &recheck, const std::function<void()> &switchPack);

}
