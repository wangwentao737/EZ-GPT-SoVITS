#include "ui.h"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ez::ui {

QString stylesheet()
{
    return QStringLiteral(R"STYLE(
        QWidget {
            color: #edf2f6;
            font-family: "Segoe UI", "Microsoft YaHei UI";
            font-size: 14px;
        }
        QWidget#workspace, QDialog, QMenu { background: #151c28; }
        QWidget#sidebar {
            background: #0d141f;
            border-right: 1px solid #263242;
        }
        QLabel#brandMark {
            color: #0a2120;
            background: #6ce1c5;
            border-radius: 10px;
            font-size: 16px;
            font-weight: 700;
        }
        QLabel#brand { color: #edf2f6; font-weight: 700; }
        QLabel#version, QLabel#muted { color: #aebcd0; }
        QLabel#version { font-size: 12px; }
        QLabel#heading { color: #f5f8fb; font-size: 27px; font-weight: 700; }
        QLabel#sectionTitle { color: #e3edf5; font-size: 15px; font-weight: 650; }
        QLabel#fieldLabel { color: #cad7e5; font-size: 13px; font-weight: 600; }
        QLabel#emptyState { color: #9cadc1; padding: 18px; }
        QLabel#engineState {
            color: #a3eedb;
            background: #172b30;
            border: 1px solid #2e494c;
            border-radius: 8px;
            padding: 8px 10px;
        }
        QLabel#stageHint {
            color: #c8eee5;
            background: #182d33;
            border: 1px solid #2b444f;
            border-radius: 8px;
            padding: 8px 11px;
        }
        QLabel#statusLine {
            color: #c4d4e6;
            border-top: 1px solid #2a3748;
            padding-top: 8px;
            min-height: 19px;
        }
        QFrame#card, QWidget#card {
            background: #1b2635;
            border: 1px solid #2e3d51;
            border-radius: 11px;
        }
        QListWidget#navigation {
            background: transparent;
            border: 0;
            outline: none;
        }
        QListWidget#navigation::item { padding: 10px 12px; min-height: 22px; margin: 2px 0; }
        QListWidget#navigation::item:hover { color: #ffffff; background: #202e40; border-radius: 8px; }
        QListWidget#navigation::item:selected {
            color: #b0f5e3;
            background: #203a43;
            border-left: 3px solid #6ce1c5;
            border-radius: 8px;
        }
        QListWidget#navigation::item:focus { outline: none; }
        QTabWidget::pane { border: 0; }
        QPushButton, QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
            border: 1px solid #3b4b60;
            border-radius: 7px;
            padding: 6px 10px;
            min-height: 21px;
        }
        QPushButton { background: #26364b; color: #eaf0f6; }
        QPushButton:hover { background: #324863; border-color: #608090; }
        QPushButton:pressed, QPushButton:checked { background: #1d3140; }
        QPushButton:disabled, QComboBox:disabled { background: #1b2533; border-color: #2a3748; color: #8799ae; }
        QPushButton#primary { background: #76dec8; color: #102922; border: 1px solid #76dec8; font-weight: 700; }
        QPushButton#primary:hover { background: #a0f0dd; border-color: #a0f0dd; }
        QPushButton#primary:pressed { background: #5bcab0; }
        QPushButton#primary:disabled { background: #27443f; border-color: #31534c; color: #a4c4bd; }
        QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
            background: #111b29;
            selection-background-color: #34776d;
            selection-color: #ffffff;
        }
        QComboBox { padding-right: 20px; }
        QComboBox QAbstractItemView { background: #1e2b3b; border: 1px solid #4e6279; padding: 4px; }
        QComboBox QAbstractItemView::item { min-height: 24px; }
        QPlainTextEdit {
            border: 1px solid #35465d;
            border-radius: 8px;
            padding: 10px;
        }
        QPlainTextEdit#scriptEditor { font-size: 16px; }
        QLineEdit[readOnly="true"] { color: #bdcbdb; }
        QPushButton:focus, QComboBox:focus, QLineEdit:focus, QPlainTextEdit:focus,
        QSpinBox:focus, QDoubleSpinBox:focus { border-color: #6cbcaf; }
        QTableWidget {
            background: #131d2b;
            alternate-background-color: #182433;
            selection-background-color: #244a55;
            selection-color: #eefcf9;
            border: 1px solid #35465d;
            border-radius: 8px;
            gridline-color: #2a384b;
        }
        QTableWidget::item { padding: 0 8px; }
        QHeaderView { background: #1e2c3d; }
        QHeaderView::section {
            background: #1e2c3d;
            color: #c6d7e8;
            border: 0;
            border-bottom: 1px solid #394a5f;
            padding: 8px;
            font-weight: 600;
        }
        QMenu { border: 1px solid #394a5f; padding: 5px; }
        QMenu::item { padding: 7px 22px; }
        QMenu::item:selected { background: #244a55; }
        QScrollBar:vertical { background: #17212e; width: 10px; margin: 0; }
        QScrollBar:horizontal { background: #17212e; height: 10px; margin: 0; }
        QScrollBar::handle { background: #455a71; border-radius: 4px; min-height: 24px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QToolTip { background: #202d3e; color: #edf2f6; border: 1px solid #506884; padding: 6px; }
    )STYLE");
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("muted");
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

void pageHeader(QVBoxLayout *layout, QWidget *page, const QString &title, const QString &description)
{
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(11);
    auto *heading = new QLabel(title, page);
    heading->setObjectName("heading");
    layout->addWidget(heading);
    layout->addWidget(hint(description, page));
}

void styleTable(QTableWidget *table)
{
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(35);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

}
