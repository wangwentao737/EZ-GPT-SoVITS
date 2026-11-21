#pragma once

#include <QJsonObject>
#include <QWidget>
#include <functional>

class QComboBox;
class QLabel;
class QMediaPlayer;
class QSpinBox;
class QTableWidget;

namespace ez {

class TaskScheduler;

class TrainingPanel final : public QWidget {
public:
    TrainingPanel(QString root, TaskScheduler &scheduler, std::function<QString()> launcher,
                  std::function<void(QJsonObject)> openVoice, QWidget *parent = nullptr);

private:
    void addProject();
    void loadProject();
    void saveProject(const QJsonObject &project);
    void importAudio(bool clips);
    void readClips();
    void loadLabels(const QString &path);
    void saveLabels();
    void submit(const QString &operation);
    void complete(const QString &id, const QJsonObject &result);
    void importVoice();
    void showState();
    bool busy() const;

    QString root_;
    TaskScheduler &scheduler_;
    std::function<QString()> launcher_;
    std::function<void(QJsonObject)> openVoice_;
    QJsonObject project_;
    QComboBox *projects_ = nullptr;
    QTableWidget *labels_ = nullptr;
    QSpinBox *gpu_ = nullptr;
    QSpinBox *batch_ = nullptr;
    QSpinBox *epochs_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *stage_ = nullptr;
    QMediaPlayer *player_ = nullptr;
};

}
