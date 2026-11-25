#pragma once

#include <QJsonObject>
#include <QWidget>
#include <functional>

class QComboBox;
class QCheckBox;
class QLabel;
class QMediaPlayer;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTableWidget;

namespace ez {

class TaskScheduler;

class TrainingPanel final : public QWidget {
    Q_OBJECT
public:
    TrainingPanel(QString root, TaskScheduler &scheduler, std::function<QString()> launcher,
                  std::function<void(QJsonObject)> openVoice, QWidget *parent = nullptr);
    void refreshEngineSelection();

signals:
    void engineDetailsRequested();
    void engineRecheckRequested();
    void engineSwitchRequested();

private:
    void addProject();
    void loadProject();
    void saveProject(const QJsonObject &project);
    void importAudio(bool clips);
    void readClips();
    void loadLabels(const QString &path, bool automatic = false);
    void saveLabels();
    void saveAndAdvance();
    void updateReviewFilter();
    QJsonObject reviewDrafts() const;
    QString clipId(const QString &filename) const;
    void submit(const QString &operation, bool resume = false);
    void complete(const QString &id, const QJsonObject &result, bool live = true);
    void importVoice();
    void showState();
    void showActivity();
    bool busy() const;
    QString nextTrainingStage() const;
    QString resumeJob(const QString &operation) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

    QString root_;
    TaskScheduler &scheduler_;
    std::function<QString()> launcher_;
    std::function<void(QJsonObject)> openVoice_;
    QJsonObject project_;
    QComboBox *projects_ = nullptr;
    QTableWidget *labels_ = nullptr;
    QCheckBox *pendingOnly_ = nullptr;
    bool reviewsDirty_ = false;
    QSpinBox *gpu_ = nullptr;
    QSpinBox *batch_ = nullptr;
    QSpinBox *epochs_ = nullptr;
    QPushButton *create_ = nullptr;
    QPushButton *recordings_ = nullptr;
    QPushButton *slice_ = nullptr;
    QPushButton *transcribe_ = nullptr;
    QPushButton *prepare_ = nullptr;
    QPushButton *train_ = nullptr;
    QPushButton *resume_ = nullptr;
    QPushButton *preview_ = nullptr;
    QPushButton *playClip_ = nullptr;
    QPushButton *saveNext_ = nullptr;
    QPushButton *suggested_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *stage_ = nullptr;
    QLabel *compatibilityStatus_ = nullptr;
    QWidget *compatibilityActions_ = nullptr;
    QMediaPlayer *player_ = nullptr;
};

}
