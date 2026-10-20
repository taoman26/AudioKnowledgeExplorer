#include "ui/Theme.h"

#include <QList>
#include <utility>

namespace ake {

Theme Theme::light()
{
    return { "#F4F5F7", "#FFFFFF", "#ECEEF2", "#D9DCE3", "#1F2430", "#5F6675", "#9AA1AF",
             "#2F6FEB", "#E3ECFD", "#EAEFF8", "#2A4F9A", "#2E9E5B", "#E0A100", "#D64545", "#FFE58A" };
}

Theme Theme::dark()
{
    return { "#16181D", "#1F2229", "#1A1D23", "#323744", "#E6E8EE", "#A6ADBB", "#6E7585",
             "#5B8DEF", "#26344F", "#2A3141", "#9DB8F0", "#4CC38A", "#F0B429", "#F06A6A", "#6B5A12" };
}

Theme Theme::forPalette(const QPalette& p)
{
    return p.color(QPalette::Window).lightness() < 128 ? dark() : light();
}

QString Theme::styleSheet() const
{
    QString css(
        "QMainWindow, QStatusBar { background: @bg@; color: @text@; }"
        "QStatusBar { border-top: 1px solid @line@; }"
        "QToolBar QToolButton { color: @text@; border: 1px solid transparent; border-radius: 5px; padding: 5px 10px; }"
        "QToolBar QToolButton:hover { background: @accentBg@; color: @accent@; }"
        "QToolBar { background: @panel@; border-bottom: 1px solid @line@; spacing: 6px; padding: 8px 12px; }"
        "QMenuBar { background: @panel@; color: @text@; }"
        "#sidebar { background: @side@; border-right: 1px solid @line@; }"
        "#cardArea { background: @bg@; }"
        "#detailPane { background: @panel@; border-left: 1px solid @line@; }"
        "QLabel#placeholder { color: @faint@; }"
        "QLabel#cardTitle { color: @text@; font-size: 17px; font-weight: bold; }"
        "QLabel#cardCount { color: @faint@; font-size: 12px; padding-top: 4px; }"
        "QListView#cardList { background: transparent; border: none; }"
        "#detailPane QScrollArea, #detailPane QWidget#detailInner, #detailPane QStackedWidget { background: transparent; border: none; }"
        "#detailBar { border-top: 1px solid @line@; background: @panel@; }"
        "QFrame#detailSeparator { background: @line@; border: none; }"
        "QLabel#detailTitle { color: @text@; font-size: 17px; font-weight: bold; }"
        "QLabel#detailSub { color: @faint@; font-size: 11px; }"
        "QLabel#detailSection { color: @faint@; font-size: 11px; font-weight: bold; }"
        "QLabel#detailSummary { color: @text@; font-size: 12px; }"
        "QLabel#detailSummary[faint=\"true\"] { color: @faint@; }"
        "QLabel#detailSummary[error=\"true\"] { color: @red@; }"
        "QLabel#badge { background: @accentBg@; color: @chipText@; border-radius: 8px; padding: 1px 8px; font-size: 10px; }"
        "QLabel#badge[user=\"true\"] { background: @accent@; color: white; }"
        "QToolButton#flatTool { border: none; color: @faint@; font-size: 15px; padding: 2px 4px; }"
        "QToolButton#flatTool[on=\"true\"] { color: @amber@; }"
        "QToolButton#flatTool:hover { color: @accent@; }"
        "QPushButton#linkButton { border: none; background: transparent; color: @accent@; font-size: 11px; padding: 2px 4px; }"
        "QPushButton#linkButton:disabled { color: @faint@; }"
        "QFrame#chip { background: @chip@; border-radius: 11px; }"
        "QFrame#chip[user=\"true\"] { background: @accentBg@; border: 1px solid @accent@; }"
        "QFrame#chip QLabel { color: @chipText@; font-size: 11px; background: transparent; }"
        "QToolButton#chipRemove { border: none; color: @faint@; font-size: 9px; padding: 0 2px; }"
        "QToolButton#chipRemove:hover { color: @red@; }"
        "QPushButton#chipAdd { color: @accent@; background: @accentBg@; border: none; border-radius: 11px; padding: 3px 11px; font-size: 11px; }"
        "QLineEdit#keywordEdit, QLineEdit#titleEdit { border-radius: 6px; padding: 3px 8px; }"
        "QPlainTextEdit { background: @bg@; color: @text@; border: 1px solid @line@; border-radius: 6px; padding: 4px; selection-background-color: @accent@; }"
        "QPlainTextEdit:focus { border: 1px solid @accent@; }"
        "QPushButton { background: @panel@; color: @text@; border: 1px solid @line@; border-radius: 5px; padding: 6px 14px; }"
        "QPushButton:hover { border: 1px solid @accent@; }"
        "QPushButton:disabled { color: @faint@; }"
        "QPushButton#primary { background: @accent@; color: white; border: none; border-radius: 5px; padding: 7px 16px; }"
        "QPushButton#primary:disabled { background: @line@; color: @faint@; }"
        "#transcriptHeader { background: @panel@; border-bottom: 1px solid @line@; }"
        "#transcriptSide { background: @side@; border-right: 1px solid @line@; }"
        "QListView#transcriptList { background: @panel@; border: none; }"
        "QToolButton#toolToggle { border: 1px solid @line@; border-radius: 5px; padding: 5px 10px; background: @panel@; color: @text@; }"
        "QToolButton#toolToggle:checked { background: @accentBg@; color: @accent@; border: 1px solid @accent@; }"
        "QToolButton#toolToggle::menu-indicator { image: none; width: 0; }"
        "QLabel#searchCounter { color: @faint@; font-size: 11px; }"
        "QPushButton#chipButton { background: @chip@; color: @chipText@; border: none; border-radius: 10px; padding: 3px 10px; font-size: 11px; }"
        "QPushButton#chipButton[user=\"true\"] { background: @accentBg@; border: 1px solid @accent@; }"
        "QPlainTextEdit#segmentEditor { background: @panel@; color: @text@; border: 2px solid @accent@; border-radius: 6px; font-size: 14px; padding: 4px; }"
        "QToolButton#viewToggle { border: 1px solid @line@; background: @panel@; color: @sub@; padding: 5px 11px; font-size: 14px; }"
        "QToolButton#viewToggle:checked { background: @accentBg@; color: @accent@; border: 1px solid @accent@; }"
        "QToolButton#viewToggle:hover { border: 1px solid @accent@; }"
        "QToolButton#sortButton { border: 1px solid @line@; border-radius: 5px; background: @panel@; color: @sub@; padding: 5px 12px; margin-right: 8px; }"
        "QToolButton#sortButton:hover { border: 1px solid @accent@; }"
        "QToolButton#sortButton::menu-indicator { image: none; width: 0; }"
        "QLabel#emptyIcon { font-size: 44px; color: @faint@; }"
        "QLabel#emptyTitle { font-size: 18px; font-weight: 600; color: @text@; }"
        "QLabel#emptyText { font-size: 13px; color: @sub@; }"
        "QPushButton#filterChip { background: @accentBg@; color: @accent@; border: 1px solid @accent@; border-radius: 12px; padding: 3px 12px; font-size: 12px; }"
        "QPushButton#filterChip:hover { background: @accent@; color: white; }"
        "QLabel#folderMessage { color: @sub@; font-size: 12px; }"
        "QWidget#modelBanner { background: @chip@; border: 1px solid @red@; border-radius: 6px; }"
        "QWidget#modelBanner QLabel { color: @text@; font-size: 12px; }"
        "QLabel#folderMessage[error=\"true\"] { color: @red@; }"
        "#miniPlayer { background: @panel@; border-top: 1px solid @line@; }"
        "QLabel#playerTitle { color: @text@; font-size: 12px; font-weight: bold; }"
        "QLabel#playerError { color: @red@; font-size: 11px; }"
        "QToolButton#playerButton, QToolButton#playerSpeed { border: 1px solid @line@; border-radius: 5px; padding: 4px 10px; background: @panel@; color: @text@; }"
        "QToolButton#playerButton:hover, QToolButton#playerSpeed:hover { border: 1px solid @accent@; }"
        "QToolButton#playerSpeed::menu-indicator { image: none; width: 0; }"
        "QToolButton#playerPlay { background: @accent@; color: white; border: none; border-radius: 18px; min-width: 36px; max-width: 36px; min-height: 36px; max-height: 36px; font-size: 14px; }"
        "QToolButton#playerClose { border: none; color: @faint@; padding: 4px; }"
        "QToolButton#playerClose:hover { color: @red@; }"
        "QSlider::groove:horizontal { background: @line@; height: 4px; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }"
        "QSlider::handle:horizontal { background: @accent@; width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }"
        "QListWidget#searchPopup { background: @panel@; border: 1px solid @line@; border-radius: 8px; outline: none; }"
        "QLineEdit { background: @inputBg@; color: @text@; border: 1px solid @line@; border-radius: 15px; padding: 5px 14px;"
        "  selection-background-color: @accent@; }"
        "QLineEdit:focus { border: 1px solid @accent@; }"
        // spin boxes and combo boxes are drawn by the platform style, which does not always honour the palette (Haiku)
        "QAbstractSpinBox, QComboBox { background: @inputBg@; color: @text@; border: 1px solid @line@; border-radius: 6px; padding: 4px 8px;"
        "  selection-background-color: @accent@; }"
        "QAbstractSpinBox:focus, QComboBox:focus { border: 1px solid @accent@; }"
        "QAbstractSpinBox QLineEdit { background: transparent; border: none; border-radius: 0; padding: 0; }"
        "QComboBox QAbstractItemView { background: @panel@; color: @text@; border: 1px solid @line@; outline: none;"
        "  selection-background-color: @accentBg@; selection-color: @accent@; }"
        "QHeaderView::section { background: @side@; color: @sub@; border: none; border-bottom: 1px solid @line@; padding: 4px 6px; }"
        "QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }"
        "QScrollBar::handle:vertical { background: @line@; border-radius: 5px; min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: @faint@; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }")
        ;
    // Named tokens, not %N: Qt's multi-arg arg() numbers by order of appearance, so one unused number shifts every colour.
    const QList<std::pair<QString, QColor>> tokens = {
        { "bg", bg }, { "text", text }, { "sub", sub }, { "line", line }, { "panel", panel }, { "side", side }, { "faint", faint },
        { "inputBg", bg }, { "accent", accent }, { "red", red }, { "accentBg", accentBg }, { "chipText", chipText },
        { "amber", amber }, { "chip", chip } };
    for (const auto& t : tokens) css.replace("@" + t.first + "@", t.second.name());
    return css;
}

} // namespace ake
