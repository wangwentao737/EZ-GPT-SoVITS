#pragma once

#include <QFutureWatcher>
#include <QJsonObject>
#include <QWidget>
#include <functional>

class QComboBox;
class QPlainTextEdit;
class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QLabel;
class QMediaPlayer;
class QPushButton;

namespace ez {

class TaskScheduler;

class SynthesisPanel final : public QWidget {
public:
    SynthesisPanel(QString root, TaskScheduler &scheduler, std::function<QString()> launcher,
                   QWidget *parent = nullptr);
    ~SynthesisPanel() override;
    void importVoice(QJsonObject defaults = {});
    void playAudio(const QString &path);
    void loadRequest(const QJsonObject &request);
    void refreshVoices(const QString &selected = {});
    bool importing() const { return importing_; }

private:
    void generate();
    void receiveAudio(const QString &id, const QJsonObject &result);

    QString root_;
    TaskScheduler &scheduler_;
    std::function<QString()> launcher_;
    QFutureWatcher<QJsonObject> importer_;
    bool importing_ = false;
    QComboBox *voices_ = nullptr;
    QComboBox *language_ = nullptr;
    QPlainTextEdit *text_ = nullptr;
    QCheckBox *batch_ = nullptr;
    QDoubleSpinBox *speed_ = nullptr;
    QLineEdit *seed_ = nullptr;
    QLabel *status_ = nullptr;
    QMediaPlayer *player_ = nullptr;
    QPushButton *play_ = nullptr;
    QPushButton *export_ = nullptr;
    QPushButton *folder_ = nullptr;
    QString audio_;
};

}
