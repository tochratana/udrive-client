// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QWidget>
#include <memory>

class QLabel;
class QComboBox;
class QScrollArea;
class QPlainTextEdit;
class QStackedWidget;
class QTemporaryDir;
class QMediaPlayer;
class QPushButton;
class QSlider;

namespace OCC {
class UDriveFilePreview : public QWidget
{
    Q_OBJECT
public:
    enum class Kind { Image, Video, Audio, Text, External, Unsupported };
    static Kind kindForName(const QString &name);
    static qint64 maximumBytes(Kind kind);
    explicit UDriveFilePreview(QWidget *parent = nullptr);
    ~UDriveFilePreview() override;
    void openFile(const QString &path, const QString &name, std::shared_ptr<QTemporaryDir> storage = {});
    void showLoading(const QString &name);
    void showError(const QString &message);
    void clear();
Q_SIGNALS:
    void backRequested();
    void downloadRequested();
    void ready();
protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void updateImage();
    quint64 _generation = 0;
    std::shared_ptr<QTemporaryDir> _storage;
    QImage _decoded;
    QLabel *_title;
    QLabel *_status;
    QLabel *_image;
    QScrollArea *_scroll;
    QPlainTextEdit *_text;
    QStackedWidget *_content;
    QComboBox *_zoom;
#ifdef UDRIVE_HAS_MULTIMEDIA
    void setVideoFullScreen(bool fullScreen);
    void markMediaReady();
    bool _mediaReady = false;
    bool _videoFullScreen = false;
    QWidget *_fullScreenWindow = nullptr;
    QMediaPlayer *_player = nullptr;
    QWidget *_video = nullptr;
    QWidget *_screen = nullptr;
    QWidget *_controlBar = nullptr;
    QSlider *_seek = nullptr;
    QLabel *_timeLabel = nullptr;
    QPushButton *_playBtn = nullptr;
    QPushButton *_fullScreenBtn = nullptr;
#endif
};
}
