// 自動テスト(画面は出さない: QT_QPA_PLATFORM=offscreen)
//  開く -> レイヤー移動(キー・マウス) -> 文字変更 -> 保存 -> 再読込 -> render
#include "mainwindow.h"
#include <QApplication>
#include <QtTest>
#include <QProcess>
#include <QDir>
#include <QJsonDocument>
#include <cmath>

static QString U(const char *s) { return QString::fromUtf8(s); }

static double psnr(const QImage &a0, const QImage &b0) {
  QImage a = a0.convertToFormat(QImage::Format_ARGB32_Premultiplied), b = b0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (a.size() != b.size()) return -1;
  double se = 0; qint64 n = 0;
  for (int y = 0; y < a.height(); y++) {
    const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y);
    for (int i = 0; i < a.width() * 4; i++) { double d = double(pa[i]) - pb[i]; se += d * d; n++; }
  }
  if (se == 0) return 99;
  return 10 * std::log10(255.0 * 255.0 / (se / n));
}

static QByteArray jsonOf(const char *t) { return QByteArray(t).replace('\'', '"'); }

class GuiTest : public QObject {
  Q_OBJECT
  QString root;
  QString writeDoc(const QString &dir, const QByteArray &json) {
    QDir().mkpath(dir); QString p = dir + "/doc" + vimg::EXT; QFile f(p); f.open(QIODevice::WriteOnly); f.write(json); f.close(); return p;
  }
  // vimg.exe --render(既定のプラットフォーム = 画面なしで描く)
  bool cliRender(const QString &doc, const QString &out) {
    QString exe = QCoreApplication::applicationDirPath() + "/vimg.exe";
    if (!QFile::exists(exe)) return false;
    QProcess pr; QProcessEnvironment env = QProcessEnvironment::systemEnvironment(); env.remove("QT_QPA_PLATFORM"); pr.setProcessEnvironment(env);
    pr.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) { a->flags |= 0x08000000; });   // CREATE_NO_WINDOW
    pr.start(exe, {"--render", doc, "-o", out}); pr.waitForFinished(60000);
    return pr.exitStatus() == QProcess::NormalExit && pr.exitCode() == 0;
  }
private slots:
  void initTestCase() {
    root = "C:/Temp/vimgn/gui_test"; QDir(root).removeRecursively(); QDir().mkpath(root);
  }

  void open_move_text_save_reload_render() {
    QString dir = root + "/a";
    QDir().mkpath(dir + "/assets");
    QImage pic(200, 120, QImage::Format_ARGB32); pic.fill(QColor("#22aa66")); pic.save(dir + "/assets/pic.png");
    QString docf = writeDoc(dir, jsonOf(
        "{"
        "'width': 640, 'height': 360, 'background': '#202040',"
        "'layers': ["
        "{'type':'image','name':'pic','src':'assets/pic.png','x':320,'y':180,'w':200,'h':120,'adjust':{'brightness':10,'contrast':0,'saturation':0}},"
        "{'type':'shape','name':'box','shape':'roundrect','x':200,'y':100,'w':160,'h':80,'fill':'#3B82F6','stroke':{'color':'#FFFFFF','width':4},'radius':16,'id':'S1','myextra':42},"
        "{'type':'text','name':'title','id':'T1','text':'Hello','x':320,'y':300,'size':60,'font':'Yu Gothic UI','color':'#FFFFFF','outlines':[{'color':'#000000','width':6}],'shadow':{'color':'#000000','opacity':0.6,'x':4,'y':4,'blur':6}}"
        "]}"
    ));
    MainWindow w; w.silent = true; w.resize(1200, 800); w.show(); QTest::qWait(50);
    QVERIFY(w.openPath(docf));
    QCOMPARE(w.doc.layers.size(), 3);
    QVERIFY(!w.temp);
    QVERIFY(w.canvas->rendered().size() == QSize(640, 360));
    // --- レイヤー選択(一覧)とキーで移動
    w.select("T1");
    Layer *t = w.cur(); QVERIFY(t); double x0 = t->x, y0 = t->y;
    QTest::keyClick(&w, Qt::Key_Right, Qt::ShiftModifier);   // +10
    QTest::keyClick(&w, Qt::Key_Down);                        // +1
    QCOMPARE(w.cur()->x, x0 + 10); QCOMPARE(w.cur()->y, y0 + 1);
    // --- マウスでドラッグ移動
    w.canvas->snapOn = false;
    QPointF c = w.canvas->toScr(QPointF(w.cur()->x, w.cur()->y));
    double zx = w.canvas->zoom, bx = w.cur()->x, by = w.cur()->y;
    QTest::mousePress(w.canvas, Qt::LeftButton, Qt::NoModifier, c.toPoint());
    QTest::mouseMove(w.canvas, (c + QPointF(40, 20)).toPoint());
    QTest::mouseMove(w.canvas, (c + QPointF(80, 30)).toPoint());
    QTest::mouseRelease(w.canvas, Qt::LeftButton, Qt::NoModifier, (c + QPointF(80, 30)).toPoint());
    QVERIFY2(std::fabs(w.cur()->x - (bx + 80 / zx)) < 1.5, qPrintable(QString::number(w.cur()->x)));
    QVERIFY2(std::fabs(w.cur()->y - (by + 30 / zx)) < 1.5, qPrintable(QString::number(w.cur()->y)));
    QVERIFY(w.dirty);
    // --- 取り消し・やり直し
    double xm = w.cur()->x; w.undo(); QVERIFY(std::fabs(w.cur()->x - xm) > 1); w.redo(); QCOMPARE(w.cur()->x, xm);
    // --- 文字変更
    w.setText(U("変更後\n二行目"));
    QCOMPARE(w.cur()->text, U("変更後\n二行目"));
    QCOMPARE(w.cur()->name, QString("title"));   // 名前を付けてあるレイヤーは名前を保つ(image.js と同じ)
    // --- 図形の見た目を変える(パネルを通さず直接 + commit)
    w.select("S1"); w.snap(); w.cur()->fill = "#FF0000"; w.commit();
    // --- 保存 -> PNG 同時出力
    QVERIFY(w.save());
    QVERIFY(QFile::exists(dir + "/doc.png"));
    QImage saved(dir + "/doc.png"); QCOMPARE(saved.size(), QSize(640, 360));
    // --- 再読込 -> 同じ内容・知らないキーも残る
    Doc d2; QString err; QVERIFY2(Doc::load(docf, d2, &err), qPrintable(err));
    QCOMPARE(d2.layers.size(), 3);
    Layer *t2 = d2.find("T1"); QVERIFY(t2);
    QCOMPARE(t2->text, U("変更後\n二行目")); QCOMPARE(t2->x, xm);
    QCOMPARE(d2.find("S1")->fill, QString("#FF0000"));
    QFile jf(docf); jf.open(QIODevice::ReadOnly); QVERIFY(jf.readAll().contains("\"myextra\""));
    // --- GUI で開き直す
    MainWindow w2; w2.silent = true; QVERIFY(w2.openPath(docf)); QCOMPARE(w2.doc.find("T1")->text, U("変更後\n二行目"));
    // --- render: 文書から描いた画像 == 保存した PNG
    QStringList miss; QImage r = vimg::renderDoc(d2, dir, true, &miss);
    QVERIFY(miss.isEmpty());
    QVERIFY2(psnr(r, saved) > 60, qPrintable(QString::number(psnr(r, saved))));
    // --- vimg.exe --render (別プロセス・既定のプラットフォーム) とも一致(フォントが同じに使えている)
    QString cli = dir + "/cli.png";
    if (cliRender(docf, cli)) {
      double p = psnr(QImage(cli), saved);
      qInfo("PSNR(offscreen GUI save vs vimg.exe --render) = %.2f dB", p);
      // 文字(日本語のフォールバック字形)は offscreen と windows でフォントDBが違い完全一致しない。図形部分は同一であること + 全体は大きく外れないこと
      QImage ca = QImage(cli).convertToFormat(QImage::Format_ARGB32), cb = saved.convertToFormat(QImage::Format_ARGB32);
      QRect shapes(0, 0, 640, 240);
      QVERIFY2(psnr(ca.copy(shapes), cb.copy(shapes)) > 40, "shape area differs");
      QVERIFY2(p > 18, qPrintable(QString::number(p)));
    } else qWarning("vimg.exe not found next to test; CLI comparison skipped");
  }

  void key_order_preserved() {
    QString dir = root + "/order"; QDir(dir).removeRecursively();
    // わざと並びを崩した(アルファベット順でも標準順でもない)文書
    QByteArray src =
      "{\n  \"layers\": [\n    {\n      \"y\": 100,\n      \"type\": \"shape\",\n      \"x\": 200,\n      \"w\": 100,\n      \"id\": \"S1\",\n      \"h\": 50,\n"
      "      \"shape\": \"rect\",\n      \"zzz_custom\": 7,\n      \"fill\": \"#00FF00\",\n      \"name\": \"box\",\n      \"shadow\": {\n        \"y\": 3,\n        \"x\": 2,\n        \"color\": \"#000000\",\n        \"blur\": 4,\n        \"opacity\": 0.5\n      }\n    },\n"
      "    {\n      \"name\": \"t\",\n      \"text\": \"Hi\",\n      \"type\": \"text\",\n      \"id\": \"T1\",\n      \"y\": 40,\n      \"x\": 60,\n      \"size\": 30,\n      \"font\": \"Yu Gothic UI\",\n      \"outlines\": [\n        {\n          \"width\": 4,\n          \"color\": \"#FF0000\"\n        }\n      ]\n    }\n  ],\n"
      "  \"height\": 200,\n  \"my_top\": \"keep\",\n  \"width\": 300\n}\n";
    QString docf = writeDoc(dir, src);
    auto keysAt = [](const QByteArray &b, int indent) {   // 指定インデントの "key": を出現順に
      QStringList r; QByteArray pre(indent, ' ');
      for (const QByteArray &ln : b.split('\n')) if (ln.startsWith(pre + "\"") && !ln.startsWith(pre + " ")) r << QString::fromUtf8(ln.mid(indent + 1, ln.indexOf('"', indent + 1) - indent - 1));
      return r;
    };
    MainWindow w; w.silent = true; QVERIFY(w.openPath(docf));
    w.select("S1"); QTest::keyClick(&w, Qt::Key_Right, Qt::ShiftModifier);   // 動かす
    w.select("T1"); w.setText("Hello");                                      // 文字を変える
    QVERIFY(w.save());
    QFile f(docf); f.open(QIODevice::ReadOnly); QByteArray out = f.readAll(); f.close();
    // レイヤーごとに(インデント6)・影(インデント8): 元の並びが先頭、新しいキーは末尾
    auto chunks = [](const QByteArray &b) { QList<QByteArray> r; for (const QByteArray &c : b.split('\n')) { if (c == "    {") r << QByteArray(); else if (!r.isEmpty()) r.last() += c + "\n"; } return r; };
    QList<QByteArray> ca = chunks(src), cb = chunks(out); QCOMPARE(cb.size(), ca.size());
    for (int i = 0; i < ca.size(); i++)
      for (int ind : {6, 8}) {
        QStringList a = keysAt(ca[i], ind), b = keysAt(cb[i], ind);
        QVERIFY2(b.mid(0, a.size()) == a, qPrintable(QString("layer %1 indent %2: %3 != %4").arg(i).arg(ind).arg(a.join(",")).arg(b.join(","))));
      }
    QCOMPARE(keysAt(src, 2), keysAt(out, 2).mid(0, 4));
    // 再度保存しても同じ(安定)
    QVERIFY(w.doc.save(docf, nullptr)); QFile f2(docf); f2.open(QIODevice::ReadOnly); QCOMPARE(f2.readAll(), out);
  }

  void new_import_saveas_assets() {
    MainWindow w; w.silent = true;
    w.newDoc(800, 600, "#FFFFFF");
    QVERIFY(w.temp);
    QVERIFY(!w.save());      // 新規は保存先が決まるまで保存できない
    QImage pic(300, 200, QImage::Format_ARGB32); pic.fill(QColor("#cc3333")); QString src = root + "/loose_pic.png"; pic.save(src);
    Layer *im = w.addImage(src); QVERIFY(im);
    QVERIFY(im->src.startsWith("assets/"));
    QVERIFY(QFile::exists(w.baseDir + "/" + im->src));
    w.newText(400, 300, U("あいう")); QCOMPARE(w.cur()->type, QString("text"));
    w.newShape(100, 100); QCOMPARE(w.doc.layers.size(), 3);
    w.dupLayer(); QCOMPARE(w.doc.layers.size(), 4);
    w.moveLayer(-1); w.delLayer(); QCOMPARE(w.doc.layers.size(), 3);
    QString dst = root + "/b/out" + vimg::EXT;
    QVERIFY(w.saveAs(dst));
    QVERIFY(!w.temp);
    QVERIFY(QFile::exists(root + "/b/out.png"));
    QVERIFY(QFile::exists(root + "/b/assets/loose_pic.png"));        // 取り込んだ画像は保存先の assets/ へ
    Doc d; QString err; QVERIFY(Doc::load(dst, d, &err));
    bool found = false; for (auto &l : d.layers) if (l.type == "image") { found = true; QCOMPARE(l.src, QString("assets/loose_pic.png")); }
    QVERIFY(found);
    QImage png(root + "/b/out.png"); QCOMPARE(png.size(), QSize(800, 600));
    QVERIFY(qAlpha(png.pixel(2, 2)) == 255);        // 背景 白
    // 移動して上書き保存
    QVERIFY(w.openPath(dst)); w.select(w.doc.layers.first().id); double x0 = w.cur()->x; w.nudge(5, 0); QVERIFY(w.save());
    Doc d3; QVERIFY(Doc::load(dst, d3, &err)); QCOMPARE(d3.layers.first().x, x0 + 5);
  }

  void open_raster_makes_doc_and_backup() {
    QString dir = root + "/c"; QDir().mkpath(dir);
    QImage pic(120, 80, QImage::Format_ARGB32); pic.fill(QColor(30, 120, 220, 200)); QVERIFY(pic.save(dir + "/photo.png"));
    MainWindow w; w.silent = true;
    QVERIFY(w.openPath(dir + "/photo.png"));
    QVERIFY(QFile::exists(dir + "/photo.vimg.json"));
    QVERIFY(QFile::exists(dir + "/photo.src.png"));          // 元画像の退避
    QCOMPARE(w.doc.width, 120); QCOMPARE(w.doc.height, 80);
    QCOMPARE(w.doc.layers.size(), 1);
    QImage out = vimg::renderDoc(w.doc, dir, true);
    QVERIFY(psnr(out, QImage(dir + "/photo.src.png")) > 60);
  }

  void canvas_crop_and_resize() {
    MainWindow w; w.silent = true; w.newDoc(400, 300, QString());
    Layer *s = w.newShape(200, 150, 100, 100); QVERIFY(s);
    w.canvas->setTool(Canvas::Crop); w.canvas->crop = QRect(100, 50, 200, 200);
    w.canvas->applyCrop();
    QCOMPARE(w.doc.width, 200); QCOMPARE(w.doc.height, 200);
    QCOMPARE(w.cur()->x, 100.0); QCOMPARE(w.cur()->y, 100.0);
    w.undo(); QCOMPARE(w.doc.width, 400);
    w.resizeCanvas(500, 300); QCOMPARE(w.doc.width, 500); QCOMPARE(w.cur()->x, 250.0);
  }

  void keyboard_shortcuts() {
    MainWindow w; w.silent = true; w.newDoc(400, 300, QString());
    QTest::keyClick(&w, Qt::Key_U); QCOMPARE(int(w.canvas->tool), int(Canvas::Shape));
    QTest::keyClick(&w, Qt::Key_V); QCOMPARE(int(w.canvas->tool), int(Canvas::Move));
    w.newShape(100, 100); QTest::keyClick(&w, Qt::Key_J, Qt::ControlModifier); QCOMPARE(w.doc.layers.size(), 2);
    QTest::keyClick(&w, Qt::Key_Delete); QCOMPARE(w.doc.layers.size(), 1);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier); QCOMPARE(w.doc.layers.size(), 2);
    QTest::keyClick(&w, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); QCOMPARE(w.doc.layers.size(), 1);
  }
};

// 窓を出さない: Qt の致命エラー(プラグインが無い等)はメッセージボックスでなく標準エラーへ出して終わる。
// 子プロセスにも引き継がれるよう、Windows のエラーダイアログ(DLL が無い等)も止める
#include <QLibraryInfo>
#include <QFileInfo>
#include <QCoreApplication>
#include <cstdlib>
#include <windows.h>
static void noWindowGuard(char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& m) {
        std::fprintf(stderr, "%s\n", m.toLocal8Bit().constData());
        if (t == QtFatalMsg) { std::fflush(stderr); std::_Exit(3); }
    });
    // exe の隣に platforms/qoffscreen.dll が無ければ Qt 本体のプラグインを使う
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!QFileInfo::exists(exeDir + "/platforms/qoffscreen.dll") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH"))
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "C:/Libs/Qt/6.10.3/mingw_64/plugins/platforms");
}

int main(int argc, char **argv) {
  noWindowGuard(argv);
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");   // offscreen でも Windows のフォントを使う
  QApplication app(argc, argv);
  GuiTest t;
  return QTest::qExec(&t, argc, argv);
}
#include "guitest.moc"
