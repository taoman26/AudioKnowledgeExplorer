#include <QLineEdit>
#include <QRegularExpression>
#include <QSplitter>
#include <QtTest>
#include <QMessageBox>

#include "ui/MainWindow.h"
#include "ui/Theme.h"

class TstMainWindow : public QObject {
    Q_OBJECT
private slots:
    void theVersionIsInTheWindowTitleAndTheAboutBox()
    {
        const QString old = QCoreApplication::applicationVersion();
        QCoreApplication::setApplicationVersion("0.1.0-alpha.9");
        ake::MainWindow w;
        QCOMPARE(w.windowTitle(), QString("AudioKnowledgeExplorer 0.1.0-alpha.9"));
        QMessageBox* box = w.createAboutBox();
        QVERIFY2(box->text().contains("0.1.0-alpha.9"), qPrintable(box->text()));
        delete box;
        QCoreApplication::setApplicationVersion(old);
    }
    void embeddedIconHasEverySizeAndTheAboutBoxShowsIt()
    {
        const QIcon icon = ake::MainWindow::appIcon();
        QVERIFY(!icon.isNull());
        QCOMPARE(icon.availableSizes().size(), 6);
        for (int n : { 16, 32, 48, 64, 128, 256 }) QVERIFY2(!icon.pixmap(n, n).isNull(), qPrintable(QString::number(n)));
        ake::MainWindow w;
        QMessageBox* box = w.createAboutBox();
        QVERIFY(!box->iconPixmap().isNull());
        QVERIFY(box->text().contains("AudioKnowledgeExplorer"));
        delete box;
    }
    void threePaneLayout()
    {
        ake::MainWindow w;
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        QCOMPARE(w.sidebar()->width(), ake::MainWindow::kSidebarWidth);
        QCOMPARE(w.detailPane()->width(), ake::MainWindow::kDetailWidth);
        QVERIFY(w.cardArea()->width() > 400);
        QVERIFY(w.searchBox()->placeholderText().contains("検索"));
    }
    void darkThemeColorsReachPanes()
    {
        ake::MainWindow w;
        w.applyTheme(ake::Theme::dark());
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const QImage img = w.grab().toImage();
        auto px = [&](QWidget* pane) { return img.pixelColor(pane->mapTo(&w, QPoint(10, 300))); };
        QCOMPARE(px(w.sidebar()), ake::Theme::dark().side);
        QCOMPARE(px(w.cardArea()), ake::Theme::dark().bg);
        QCOMPARE(px(w.detailPane()), ake::Theme::dark().panel);
        w.applyTheme(ake::Theme::light());
        QCOMPARE(w.grab().toImage().pixelColor(w.sidebar()->mapTo(&w, QPoint(10, 300))), ake::Theme::light().side);
    }
    void styleSheetsHaveEveryTokenFilledWithTheRightColour()
    {
        for (const ake::Theme& t : { ake::Theme::light(), ake::Theme::dark() }) {
            const QString css = t.styleSheet();
            QVERIFY2(!css.contains('@'), "an unreplaced @token@ is left");
            QVERIFY2(!css.contains(QRegularExpression("%\\d")), "a leftover %N placeholder");
            auto rule = [&](const QString& selector) {          // the declaration block of one rule
                const int i = css.indexOf(selector + " {");
                return i < 0 ? QString() : css.mid(i, css.indexOf('}', i) - i);
            };
            QVERIFY2(rule("QFrame#chip").contains("background: " + t.chip.name()), qPrintable(rule("QFrame#chip")));
            QVERIFY2(rule("QFrame#chip QLabel").contains("color: " + t.chipText.name()), qPrintable(rule("QFrame#chip QLabel")));
            QVERIFY2(rule("QLabel#badge").contains("background: " + t.accentBg.name()), qPrintable(rule("QLabel#badge")));
            QVERIFY2(rule("QLabel#badge[user=\"true\"]").contains("background: " + t.accent.name()), qPrintable(rule("QLabel#badge[user=\"true\"]")));
            QVERIFY2(rule("QToolButton#flatTool[on=\"true\"]").contains("color: " + t.amber.name()), qPrintable(rule("QToolButton#flatTool[on=\"true\"]")));
            QVERIFY2(rule("#detailPane").contains("background: " + t.panel.name()), qPrintable(rule("#detailPane")));
            QVERIFY2(rule("#sidebar").contains("background: " + t.side.name()), qPrintable(rule("#sidebar")));
        }
    }
    void themeFollowsPalette()
    {
        QPalette dark; dark.setColor(QPalette::Window, QColor("#202020"));
        QPalette light; light.setColor(QPalette::Window, QColor("#f0f0f0"));
        QCOMPARE(ake::Theme::forPalette(dark).bg, ake::Theme::dark().bg);
        QCOMPARE(ake::Theme::forPalette(light).bg, ake::Theme::light().bg);
    }
};

QTEST_MAIN(TstMainWindow)
#include "tst_mainwindow.moc"
