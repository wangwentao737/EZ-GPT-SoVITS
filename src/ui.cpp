#include "ui.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QStyleHints>
#include <QTableWidget>
#include <QVBoxLayout>
#include <functional>

namespace ez::ui {
namespace {

class TableRowMenu final : public QObject {
public:
    TableRowMenu(QTableWidget *table, const QList<QAbstractButton *> &buttons, const QList<QAction *> &actions)
        : QObject(table), table_(table), buttons_(buttons), actions_(actions)
    {
        table_->installEventFilter(this);
        table_->viewport()->installEventFilter(this);
    }

private:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::ContextMenu)
            return QObject::eventFilter(watched, event);

        auto *context = static_cast<QContextMenuEvent *>(event);
        const auto keyboard = context->reason() == QContextMenuEvent::Keyboard;
        const auto index = keyboard ? table_->currentIndex()
            : table_->indexAt(table_->viewport()->mapFromGlobal(context->globalPos()));
        if (!index.isValid())
            return true;

        table_->setCurrentCell(index.row(), index.column(), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        const QPersistentModelIndex selected(index);
        QMenu menu(table_);
        QMap<QAction *, std::function<void()>> operations;
        for (auto *button : buttons_) {
            if (button->isEnabled() && button->isVisible()) {
                const QPointer<QAbstractButton> control(button);
                operations.insert(menu.addAction(button->text()), [control] {
                    if (control && control->isEnabled() && control->isVisible())
                        control->click();
                });
            }
        }
        for (auto *action : actions_) {
            if (action->isEnabled() && action->isVisible()) {
                const QPointer<QAction> control(action);
                operations.insert(menu.addAction(action->text()), [control] {
                    if (control && control->isEnabled())
                        control->trigger();
                });
            }
        }
        if (operations.isEmpty())
            return true;

        // A background refresh can rebuild the table while the menu is open.
        connect(table_->model(), &QAbstractItemModel::rowsAboutToBeRemoved, &menu, &QMenu::close);
        connect(table_->model(), &QAbstractItemModel::modelAboutToBeReset, &menu, &QMenu::close);
        if (keyboard)
            table_->scrollTo(index);
        const auto anchor = keyboard ? table_->viewport()->mapToGlobal(table_->visualRect(index).bottomLeft())
                                     : context->globalPos();
        const auto chosen = menu.exec(anchor);
        const auto operation = operations.constFind(chosen);
        if (operation != operations.cend() && selected.isValid() && selectedRow(table_) == selected.row())
            operation.value()();
        return true;
    }

    QTableWidget *table_;
    QList<QAbstractButton *> buttons_;
    QList<QAction *> actions_;
};

}

namespace {

using Colors = QMap<QString, QString>;
const Colors &themeColors(Theme theme)
{
    static const Colors dark{
        {"canvas", "#111922"}, {"rail", "#0c131b"}, {"surface", "#18232f"}, {"field", "#131d28"},
        {"line", "#304052"}, {"text", "#edf3f9"}, {"muted", "#aabbd0"}, {"disabled", "#8596a8"},
        {"button", "#223242"}, {"hover", "#293e50"}, {"pressed", "#1c2e3a"}, {"scroll", "#536a7e"},
        {"accent", "#71deca"}, {"accentHover", "#9cebdc"}, {"accentInk", "#0e2a25"},
        {"wash", "#173432"}, {"washInk", "#b1eddf"}, {"selection", "#285c5b"}, {"selectionInk", "#effffc"},
        {"focus", "#71deca"}, {"success", "#8fe1b5"}, {"danger", "#ffb0a9"},
    };
    static const Colors light{
        {"canvas", "#f3f6f9"}, {"rail", "#eaf0f5"}, {"surface", "#ffffff"}, {"field", "#ffffff"},
        {"line", "#c3d0db"}, {"text", "#172938"}, {"muted", "#475d70"}, {"disabled", "#64788a"},
        {"button", "#edf2f7"}, {"hover", "#dce9ee"}, {"pressed", "#cedfe6"}, {"scroll", "#8298aa"},
        {"accent", "#11766b"}, {"accentHover", "#0b6258"}, {"accentInk", "#ffffff"},
        {"wash", "#e0f2ed"}, {"washInk", "#155c52"}, {"selection", "#cceee5"}, {"selectionInk", "#133e37"},
        {"focus", "#176e63"}, {"success", "#196346"}, {"danger", "#9e3028"},
    };
    return theme == Theme::Light ? light : dark;
}

Theme activeTheme = Theme::Dark;

class TaskStatusDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const auto state = index.data(Qt::UserRole).toString();
        const auto &colors = themeColors(activeTheme);
        if (state == "succeeded")
            option->palette.setColor(QPalette::Text, QColor(colors.value("success")));
        else if (state == "failed" || state == "interrupted")
            option->palette.setColor(QPalette::Text, QColor(colors.value("danger")));
    }
};

QString stylesheet(const Colors &colors)
{
    auto style = QStringLiteral(R"STYLE(
        QWidget {
            color: @text@;
            font-family: "Segoe UI", "Microsoft YaHei UI";
            font-size: 14px;
        }
        QWidget#workspace, QWidget#settingsBody, QDialog, QMenu { background: @canvas@; }
        QWidget#sidebar { background: @rail@; border-right: 1px solid @line@; }
        QLabel#brand { font-size: 15px; font-weight: 700; }
        QLabel#version, QLabel#muted { color: @muted@; }
        QLabel#version { font-size: 12px; }
        QLabel#heading { font-size: 27px; font-weight: 700; }
        QLabel#sectionTitle { font-size: 15px; font-weight: 650; }
        QLabel#fieldLabel { color: @muted@; font-size: 13px; font-weight: 600; }
        QLabel#emptyState { color: @muted@; padding: 16px; }
        QLabel#engineState, QLabel#stageHint {
            color: @washInk@;
            background: @wash@;
            border-radius: 8px;
            padding: 8px 11px;
        }
        QLabel#statusLine { color: @muted@; border-top: 1px solid @line@; padding-top: 8px; }
        QFrame#card, QWidget#card {
            background: @surface@;
            border: 1px solid @line@;
            border-radius: 10px;
        }
        QListWidget#navigation { background: transparent; border: 0; outline: none; }
        QListWidget#navigation::item {
            border-left: 3px solid transparent;
            border-radius: 7px;
            padding: 8px 11px;
            min-height: 21px;
            margin: 2px 0;
        }
        QListWidget#navigation::item:hover { background: @hover@; }
        QListWidget#navigation::item:selected {
            color: @washInk@;
            background: @wash@;
            border-left: 3px solid @accent@;
        }
        QListWidget#navigation::item:focus { outline: none; }
        QTabWidget::pane, QScrollArea { border: 0; }
        QPushButton, QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
            border: 1px solid @line@;
            border-radius: 7px;
            padding: 5px 9px;
            min-height: 21px;
        }
        QPushButton { background: @button@; }
        QPushButton:hover { background: @hover@; border-color: @focus@; }
        QPushButton:pressed, QPushButton:checked { background: @pressed@; }
        QPushButton:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
            background: @canvas@; border-color: @line@; color: @disabled@;
        }
        QPushButton#primary {
            background: @accent@; color: @accentInk@; border-color: @accent@; font-weight: 700;
        }
        QPushButton#primary:hover { background: @accentHover@; border-color: @accentHover@; }
        QPushButton#primary:disabled { background: @canvas@; border-color: @line@; color: @disabled@; }
        QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit {
            background: @field@;
            selection-background-color: @selection@;
            selection-color: @selectionInk@;
        }
        QComboBox { padding-right: 23px; combobox-popup: 0; combobox-list-mousetracking: 1; }
        QComboBox::drop-down { border: 0; width: 22px; }
        QComboBox::down-arrow { image: url(:/controls/chevron-down.png); width: 11px; height: 8px; }
        QComboBox QAbstractItemView {
            background: @surface@;
            border: 1px solid @line@;
            padding: 3px;
            outline: 0;
        }
        QComboBox QAbstractItemView::item { min-height: 28px; padding: 0 6px; }
        QComboBox QAbstractItemView::item:hover:!selected { background: @hover@; }
        QComboBox QAbstractItemView::item:selected { background: @selection@; color: @selectionInk@; }
        QSpinBox, QDoubleSpinBox { padding-right: 24px; }
        QAbstractSpinBox QLineEdit { border: 0; padding: 0; min-height: 0; background: transparent; }
        QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
            border: 0; width: 21px;
        }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
            image: url(:/controls/chevron-up.png); width: 9px; height: 6px;
        }
        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
            image: url(:/controls/chevron-down.png); width: 9px; height: 6px;
        }
        QPlainTextEdit {
            border: 1px solid @line@;
            border-radius: 8px;
            padding: 9px;
        }
        QPlainTextEdit#scriptEditor { font-size: 16px; }
        QLineEdit[readOnly="true"] { color: @muted@; }
        QPushButton:focus, QComboBox:focus, QLineEdit:focus, QPlainTextEdit:focus,
        QSpinBox:focus, QDoubleSpinBox:focus { border-color: @focus@; }
        QCheckBox { spacing: 6px; }
        QCheckBox::indicator {
            background: @field@;
            border: 1px solid @scroll@;
            border-radius: 4px;
            width: 14px;
            height: 14px;
        }
        QCheckBox::indicator:checked { background: #29675d; image: url(:/controls/checkmark.png); }
        QCheckBox::indicator:focus { border-color: @focus@; }
        QTableWidget {
            background: @surface@;
            alternate-background-color: @canvas@;
            selection-background-color: @selection@;
            selection-color: @selectionInk@;
            border: 1px solid @line@;
            border-radius: 8px;
            gridline-color: @line@;
        }
        QTableWidget::item { padding: 0 7px; }
        QTableWidget::item:hover:!selected { background: @hover@; }
        QTableWidget::item:selected { background: @selection@; color: @selectionInk@; }
        QHeaderView { background: @button@; }
        QHeaderView::section {
            background: @button@;
            color: @muted@;
            border: 0;
            border-bottom: 1px solid @line@;
            padding: 7px 9px;
            font-weight: 600;
        }
        QMenu { background: @surface@; border: 1px solid @line@; padding: 4px; }
        QMenu::item { padding: 6px 20px; }
        QMenu::item:selected { background: @selection@; color: @selectionInk@; }
        QScrollBar:vertical { background: @canvas@; width: 10px; margin: 0; }
        QScrollBar:horizontal { background: @canvas@; height: 10px; margin: 0; }
        QScrollBar::handle { background: @scroll@; border-radius: 4px; min-height: 24px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
        QToolTip { background: @surface@; color: @text@; border: 1px solid @line@; padding: 5px; }
    )STYLE");
    for (auto item = colors.cbegin(); item != colors.cend(); ++item)
        style.replace('@' + item.key() + '@', item.value());
    return style;
}

}

void applyTheme(Theme theme)
{
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
    activeTheme = theme;
    const auto &colors = themeColors(theme);
    QPalette palette;
    const auto role = [&](QPalette::ColorRole role, const QString &name) {
        palette.setColor(role, QColor(colors.value(name)));
    };
    role(QPalette::Window, "canvas");
    role(QPalette::Base, "field");
    role(QPalette::AlternateBase, "canvas");
    role(QPalette::Button, "button");
    role(QPalette::WindowText, "text");
    role(QPalette::Text, "text");
    role(QPalette::ButtonText, "text");
    role(QPalette::PlaceholderText, "muted");
    role(QPalette::Highlight, "selection");
    role(QPalette::HighlightedText, "selectionInk");
    role(QPalette::ToolTipBase, "surface");
    role(QPalette::ToolTipText, "text");
    role(QPalette::Link, "success");
    for (const auto text : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, text, QColor(colors.value("disabled")));
    qApp->styleHints()->setColorScheme(theme == Theme::Light ? Qt::ColorScheme::Light : Qt::ColorScheme::Dark);
    qApp->setPalette(palette);
    qApp->setStyleSheet(stylesheet(colors));
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
    layout->setSpacing(10);
    auto *heading = new QLabel(title, page);
    heading->setObjectName("heading");
    layout->addWidget(heading);
    if (!description.isEmpty())
        layout->addWidget(hint(description, page));
}

void fitComboBox(QComboBox *box, int minimumCharacters)
{
    box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    box->setMinimumContentsLength(minimumCharacters);
    box->view()->setTextElideMode(Qt::ElideRight);
}

void styleTable(QTableWidget *table)
{
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setMouseTracking(true);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(34);
    table->horizontalHeader()->setHighlightSections(false);
    table->setTextElideMode(Qt::ElideRight);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

void styleTaskStatus(QTableWidget *table, int column)
{
    table->setItemDelegateForColumn(column, new TaskStatusDelegate(table));
}

int selectedRow(const QTableWidget *table)
{
    const auto rows = table->selectionModel()->selectedRows();
    return rows.isEmpty() ? -1 : rows.constFirst().row();
}

void enableRowMenu(QTableWidget *table, const QList<QAbstractButton *> &buttons,
                   const QList<QAction *> &actions)
{
    new TableRowMenu(table, buttons, actions);
}

QWidget *engineCompatibilityActions(QWidget *parent, const std::function<void()> &details,
                                    const std::function<void()> &recheck, const std::function<void()> &switchPack)
{
    auto *widget = new QWidget(parent);
    auto *row = new QHBoxLayout(widget);
    row->setContentsMargins(0, 0, 0, 0);
    const auto add = [widget, row](const QString &label, const std::function<void()> &action) {
        auto *button = new QPushButton(label, widget);
        row->addWidget(button);
        QObject::connect(button, &QPushButton::clicked, widget, action);
    };
    add(QStringLiteral("查看引擎详情"), details);
    add(QStringLiteral("重新检测"), recheck);
    add(QStringLiteral("切换整合包"), switchPack);
    row->addStretch();
    return widget;
}

}
