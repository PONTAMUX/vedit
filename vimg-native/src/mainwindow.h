#pragma once
// 編集画面。web 版(image.html / image.js / image.css)と同じ並び・用語・ショートカット。
#include "model.h"
#include "render.h"
#include <QMainWindow>
#include <QTemporaryDir>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QVBoxLayout>
#include <functional>
#include <memory>

class MainWindow;
using vimg::fitText;
QString pickImageFile(QWidget *parent);   // 画像を選ぶダイアログ(dialogs.cpp)

// ---- キャンバス(描画 + オーバーレイ: ハンドル・ガイド・切り抜き) ----
class Canvas : public QWidget {
  Q_OBJECT
public:
  enum Tool { Move, Text, Shape, Crop, Hand };
  explicit Canvas(MainWindow *mw);
  Tool tool = Move;
  int shapeKind = 0;                 // 0 rect / 1 roundrect / 2 ellipse
  bool snapOn = true;
  double zoom = 1; bool autoFit = true;
  QPointF origin;                    // 文書の (0,0) のウィジェット座標
  bool hasCrop = false; QRect crop;
  QList<QPair<bool, double>> guides;   // (縦線か, 位置)
  void setTool(Tool t);
  void fitView();
  void zoomAt(double f, QPointF at = QPointF(-1, -1));
  void invalidate();                 // 描き直す
  void applyCrop();
  QPointF toDoc(QPointF s) const { return (s - origin) / zoom; }
  QPointF toScr(QPointF d) const { return d * zoom + origin; }
  void startEdit(const QString &id, bool selectAll);
  void finishEdit();
  bool editing() const { return ed != nullptr; }
  // 変形の幾何(文書座標)
  static QPointF lpt(const Layer &l, double lx, double ly);       // レイヤー座標 -> 文書
  static QPointF toLocal(const Layer &l, double x, double y);     // 文書 -> レイヤー座標
  static QPointF extent(const Layer &l);
  Layer *hitLayer(double x, double y);
  QImage rendered() { ensureRender(); return cache; }
signals:
  void toolChanged();
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  bool eventFilter(QObject *, QEvent *) override;
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;
private:
  struct Handle { bool rot = false; int hx = 0, hy = 0; bool ok = false; };
  Handle handleAt(const Layer *l, QPointF s) const;
  void ensureRender();
  void drawOverlay(QPainter &p);
  QPointF snapPos(const Layer &l, double nx, double ny);
  MainWindow *mw;
  QImage cache; bool cacheOk = false;
  // ドラッグ状態
  enum Mode { None, Pan, Moving, Handling, Rotating, Drawing, Cropping } mode = None;
  QPointF p0, s0, pan0; QString dragId; Layer o0; Handle hd; bool moved = false, spaceDown = false;
  QPointF crop0;
  QPlainTextEdit *ed = nullptr; QString edId, edOrig; bool edSnapped = false;
  friend class MainWindow;
};

// ---- レイヤー一覧 ----
class LayerList : public QWidget {
  Q_OBJECT
public:
  explicit LayerList(MainWindow *mw);
  QSize sizeHint() const override { return QSize(300, 200); }
  void sync();                 // 行数に合わせて高さを直して描き直す
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
private:
  int rowAt(int y) const;       // 表示順(上=前面)の行
  MainWindow *mw; int dragRow = -1, dropAt = -1, dropRow = -1; bool dropBefore = false; QPoint press;
};

// ---- 右パネル: プロパティ ----
class PropsPanel : public QScrollArea {
  Q_OBJECT
public:
  explicit PropsPanel(MainWindow *mw);
  void rebuild();
  void refreshValues() {}
private:
  MainWindow *mw; QWidget *body = nullptr; void *lastKey = nullptr;
  void edit(void *key, const std::function<void(Layer &)> &f, bool structural = false);
};

// ---- 本体 ----
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget *parent = nullptr);
  ~MainWindow() override;
  // 状態(テストから触る)
  Doc doc; QString docPath; QString baseDir; bool temp = true, dirty = false;
  QString sel;
  QList<QByteArray> undoS, redoS;
  // 文書の操作
  bool newDoc(int w, int h, const QString &bg);
  bool openPath(const QString &path);
  bool save();                                  // 新規は名前を付けて保存
  bool saveAs(const QString &docFile);          // .vimg.json と透過PNG を書く
  QString importImage(const QString &file);     // assets/ に取り込み、文書からの相対パスを返す
  Layer *addImage(const QString &file, QPointF at = QPointF(-1, -1));
  Layer *addLayer(Layer l, bool above = true);
  Layer *newText(double x, double y, const QString &text = QString::fromUtf8("テキスト"));
  Layer *newShape(double x, double y, double w = 400, double h = 240);
  Layer *cur();
  void select(const QString &id);
  void snap();                                  // 取り消し用に今の状態を積む
  void commit(bool props = true);               // 変更を画面へ反映
  void undo(); void redo();
  void delLayer(); void dupLayer(); void moveLayer(int d);
  void copyLayers(); void pasteLayers();
  void alignLayer(int ax, int ay);              // ax/ay: 0,1,2 / -1 は変えない
  void setText(const QString &t);
  void nudge(double dx, double dy);
  void resizeCanvas(int w, int h);
  void showStart(const QString &dir = QString());
  void openDialog(); void saveAsDialog(); void newDialog();
  bool discardOk();
  void toast(const QString &s, int ms = 2600);
  QLabel *toastLbl = nullptr;
  void updateTitle();
  Canvas *canvas; LayerList *list; PropsPanel *props;
  bool silent = false;                          // テスト用: ダイアログを出さない
  QJsonObject lastTextStyle;
protected:
  void keyPressEvent(QKeyEvent *) override;
  void keyReleaseEvent(QKeyEvent *) override;
  void closeEvent(QCloseEvent *) override;
private:
  void buildUi();
  void loadFrom(const Doc &d, const QString &path, bool isTemp);
  QPushButton *mkBtn(const QString &t, const QString &tip, QWidget *parent);
  void renderOpts();
  QLabel *pname, *savestate, *stZoom, *stSize, *stPos, *optHint; QWidget *opts; QPushButton *toolBtn[5];
  QHBoxLayout *optsLay;
  std::unique_ptr<QTemporaryDir> tmp;
  QList<QJsonObject> clip; int clipN = 0;
  friend class Canvas; friend class LayerList; friend class PropsPanel;
};
