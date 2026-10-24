// vaudio: Qt GUI(チェーン編集・VST3 エディタ・処理実行・波形)
#pragma once
#include <QMainWindow>
#include <QString>
#include <memory>
#include <string>

#include "json.h"

class QListWidget;
class QFormLayout;
class QWidget;
class QProgressBar;
class QLabel;
class QComboBox;
class WaveView;

namespace vaudio {
class Vst3Plugin;
class Vst3Editor;
}

class AudioWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit AudioWindow(QWidget* parent = nullptr);
    ~AudioWindow() override;

    bool loadProject(const QString& path, QString* err = nullptr);   // .vedit.json を読む(audio 以外は触らない)
    bool saveProject(QString* err = nullptr);                        // audio だけを差し替えて保存
    const vj::Value& chain() const { return chain_; }
    int currentRow() const;
    void selectRow(int n);

    // 編集操作(ボタンと同じ。テストからも呼ぶ)。追加は normalize の手前に入れる(app.js と同じ)
    void addDeclick();
    void addGain();
    void addVst3(const QString& path);
    void addNormalize();
    void addPlugin(const QString& name);
    void applyVoicePreset();
    void removeRow(int n);
    void moveRow(int n, int delta);
    void setStepNumber(int n, const QString& key, double v);   // st[key] か st.params[key]
    void setNormalizeMode(int n, bool lufs);
    void runProcess();                                          // 保存して処理(別スレッド)
    bool processing() const { return busy_; }
    void openVst3Editor(int n);

signals:
    void processFinished(bool ok, QString message);

private:
    void rebuildList();
    void rebuildDetail();
    void insertStep(vj::Value st);
    void markStale(bool s = true);
    void refreshWaves();
    QString stepTitle(const vj::Value& st) const;

    std::string path_;
    vj::Value doc_;     // プロジェクト全体
    vj::Value chain_;   // audio.chain の編集中の写し。保存時に書き戻す
    bool stale_ = false, busy_ = false;
    QListWidget* list_ = nullptr;
    QWidget* detail_ = nullptr;
    QFormLayout* form_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* note_ = nullptr;
    QComboBox* fxBox_ = nullptr;
    WaveView* before_ = nullptr;
    WaveView* after_ = nullptr;
    QWidget* editorWin_ = nullptr;
    std::unique_ptr<vaudio::Vst3Plugin> editorPlugin_;
    std::unique_ptr<vaudio::Vst3Editor> editor_;
};

int runGui(const std::string& file);
