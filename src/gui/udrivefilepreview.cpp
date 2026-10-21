// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivefilepreview.h"
#include "udriveicons.h"
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSlider>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#ifdef UDRIVE_HAS_MULTIMEDIA
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QShortcut>
#include <QStyle>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVideoWidget>

static QIcon playerIcon(OCC::UDriveIcons::Symbol symbol)
{
    return OCC::UDriveIcons::icon(symbol, false, QColor(255, 255, 255));
}

static QString formatMediaTime(qint64 ms)
{
    if (ms <= 0) return QStringLiteral("00:00");
    const qint64 totalSeconds = (ms + 500) / 1000;
    const qint64 seconds = totalSeconds % 60;
    const qint64 minutes = (totalSeconds / 60) % 60;
    const qint64 hours = totalSeconds / 3600;
    if (hours > 0) {
        return QString::asprintf("%lld:%02lld:%02lld", hours, minutes, seconds);
    }
    return QString::asprintf("%02lld:%02lld", minutes, seconds);
}
#endif

namespace OCC {
UDriveFilePreview::Kind UDriveFilePreview::kindForName(const QString &name)
{
    const QString extension = QFileInfo(name).suffix().toLower();
    if (QStringList{"png", "jpg", "jpeg", "gif", "webp", "bmp"}.contains(extension)) return Kind::Image;
    if (QStringList{"mp4", "m4v", "mov", "webm", "mkv", "avi", "wmv"}.contains(extension)) return Kind::Video;
    if (QStringList{"mp3", "wav", "m4a", "flac", "ogg"}.contains(extension)) return Kind::Audio;
    if (QStringList{"txt", "md", "csv", "log", "json", "xml", "yaml", "yml", "py", "js", "ts", "jsx", "tsx", "c", "cpp", "cc", "cxx", "h", "hpp", "sh", "bash", "zsh", "ini", "conf", "sql", "html", "htm", "css", "scss", "java", "rs", "go"}.contains(extension)) return Kind::Text;
    if (QStringList{"doc", "docx", "odt", "xls", "xlsx", "ods", "ppt", "pptx", "odp", "rtf"}.contains(extension)) return Kind::External;
    return Kind::Unsupported;
}

qint64 UDriveFilePreview::maximumBytes(Kind kind)
{
    if (kind == Kind::Video) return 512LL * 1024 * 1024;
    if (kind == Kind::Audio) return 100LL * 1024 * 1024;
    if (kind == Kind::Text) return 2LL * 1024 * 1024;
    if (kind == Kind::External) return 100LL * 1024 * 1024;
    return 32LL * 1024 * 1024;
}

UDriveFilePreview::UDriveFilePreview(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("driveFilePreview"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    auto *header = new QHBoxLayout;
    auto *back = new QPushButton(tr("Back to files"), this);
    back->setObjectName(QStringLiteral("previewBack"));
    back->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    header->addWidget(back);
    _title = new QLabel(this);
    _title->setTextFormat(Qt::PlainText);
    _title->setWordWrap(true);
    header->addWidget(_title, 1);
    _zoom = new QComboBox(this);
    _zoom->setObjectName(QStringLiteral("previewZoom"));
    _zoom->setAccessibleName(tr("Image zoom"));
    _zoom->addItem(tr("Fit page"), 0);
    for (int percent : {50, 100, 150, 200}) _zoom->addItem(tr("%1%").arg(percent), percent);
    header->addWidget(_zoom);
    auto *download = new QPushButton(tr("Download"), this);
    download->setObjectName(QStringLiteral("previewDownload"));
    header->addWidget(download);
    layout->addLayout(header);
    _status = new QLabel(this);
    _status->setObjectName(QStringLiteral("previewStatus"));
    _status->setTextFormat(Qt::PlainText);
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _content = new QStackedWidget(this);
    layout->addWidget(_content, 1);
    _content->addWidget(new QWidget(_content));
    _scroll = new QScrollArea(_content);
    _scroll->setAlignment(Qt::AlignCenter);
    _image = new QLabel;
    _image->setObjectName(QStringLiteral("previewImage"));
    _image->setScaledContents(true);
    _scroll->setWidget(_image);
    _content->addWidget(_scroll);
    _text = new QPlainTextEdit(_content);
    _text->setObjectName(QStringLiteral("previewText"));
    _text->setReadOnly(true);
    _content->addWidget(_text);
#ifdef UDRIVE_HAS_MULTIMEDIA
    _video = new QWidget(_content);
    _video->setObjectName(QStringLiteral("previewVideoContainer"));
    _video->installEventFilter(this);
    auto *videoLayout = new QVBoxLayout(_video);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    videoLayout->setSpacing(0);
    _screen = new QVideoWidget(_video);
    _screen->setObjectName(QStringLiteral("previewVideo"));
    _screen->setFocusPolicy(Qt::StrongFocus);
    _screen->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    _screen->setStyleSheet(QStringLiteral("QVideoWidget { background-color: #000000; border-top-left-radius: 8px; border-top-right-radius: 8px; }"));
    _screen->installEventFilter(this);
    videoLayout->addWidget(_screen, 1);

    _controlBar = new QWidget(_video);
    _controlBar->setObjectName(QStringLiteral("previewControlBar"));
    // Keep the timeline and buttons together when the video surface is resized
    // or restored from fullscreen, including before its size is known.
    _controlBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    _controlBar->setFixedHeight(64);
    _controlBar->setStyleSheet(QStringLiteral(
        "QWidget#previewControlBar {"
        "    background-color: #0f0f0f;"
        "    border-bottom-left-radius: 8px;"
        "    border-bottom-right-radius: 8px;"
        "    border-top: 1px solid rgba(255, 255, 255, 0.08);"
        "}"
    ));
    auto *barLayout = new QVBoxLayout(_controlBar);
    barLayout->setContentsMargins(12, 0, 12, 6);
    barLayout->setSpacing(2);

    _seek = new QSlider(Qt::Horizontal, _controlBar);
    _seek->setObjectName(QStringLiteral("previewSeek"));
    _seek->setRange(0, 1000);
    _seek->setFixedHeight(18);
    _seek->setAccessibleName(tr("Playback position"));
    _seek->setCursor(Qt::PointingHandCursor);
    _seek->installEventFilter(this);
    _seek->setStyleSheet(QStringLiteral(
        "QSlider#previewSeek {"
        "    height: 18px;"
        "    background: transparent;"
        "}"
        "QSlider#previewSeek::groove:horizontal {"
        "    height: 3px;"
        "    background: rgba(255, 255, 255, 0.28);"
        "    border-radius: 1px;"
        "}"
        "QSlider#previewSeek::sub-page:horizontal {"
        "    background: #ffffff;"
        "    border-radius: 1px;"
        "}"
        "QSlider#previewSeek::handle:horizontal {"
        "    background: #ffffff;"
        "    width: 10px;"
        "    height: 10px;"
        "    margin-top: -3.5px;"
        "    margin-bottom: -3.5px;"
        "    border-radius: 5px;"
        "}"
        "QSlider#previewSeek::handle:horizontal:hover {"
        "    background: #ffffff;"
        "    width: 14px;"
        "    height: 14px;"
        "    margin-top: -5.5px;"
        "    margin-bottom: -5.5px;"
        "    border-radius: 7px;"
        "}"
    ));
    barLayout->addWidget(_seek);

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(6);

    const QString btnStyle = QStringLiteral(
        "QPushButton {"
        "    background: transparent;"
        "    border: none;"
        "    border-radius: 15px;"
        "    padding: 0;"
        "    margin: 0;"
        "}"
        "QPushButton:hover {"
        "    background: rgba(255, 255, 255, 0.2);"
        "}"
        "QPushButton:pressed {"
        "    background: rgba(255, 255, 255, 0.32);"
        "}"
    );

    _playBtn = new QPushButton(_controlBar);
    _playBtn->setObjectName(QStringLiteral("previewPlay"));
    _playBtn->setIcon(playerIcon(UDriveIcons::Symbol::Play));
    _playBtn->setIconSize(QSize(18, 18));
    _playBtn->setToolTip(tr("Play (Space)"));
    _playBtn->setAccessibleName(tr("Play"));
    _playBtn->setFixedSize(30, 30);
    _playBtn->setCursor(Qt::PointingHandCursor);
    _playBtn->setStyleSheet(btnStyle);

    auto *rewind10 = new QPushButton(_controlBar);
    rewind10->setObjectName(QStringLiteral("previewRewind10"));
    rewind10->setIcon(playerIcon(UDriveIcons::Symbol::Rewind10));
    rewind10->setIconSize(QSize(24, 24));
    rewind10->setToolTip(tr("Back 10 seconds"));
    rewind10->setAccessibleName(tr("Rewind 10 seconds"));
    rewind10->setAccessibleDescription(tr("Left arrow or J"));
    rewind10->setFixedSize(36, 36);
    rewind10->setCursor(Qt::PointingHandCursor);
    rewind10->setStyleSheet(btnStyle);

    auto *forward10 = new QPushButton(_controlBar);
    forward10->setObjectName(QStringLiteral("previewForward10"));
    forward10->setIcon(playerIcon(UDriveIcons::Symbol::Forward10));
    forward10->setIconSize(QSize(24, 24));
    forward10->setToolTip(tr("Forward 10 seconds"));
    forward10->setAccessibleName(tr("Forward 10 seconds"));
    forward10->setAccessibleDescription(tr("Right arrow or L"));
    forward10->setFixedSize(36, 36);
    forward10->setCursor(Qt::PointingHandCursor);
    forward10->setStyleSheet(btnStyle);

    _timeLabel = new QLabel(QStringLiteral("00:00 / 00:00"), _controlBar);
    _timeLabel->setObjectName(QStringLiteral("previewTime"));
    _timeLabel->setTextFormat(Qt::PlainText);
    _timeLabel->setStyleSheet(QStringLiteral("QLabel { color: #f0f0f0; font-size: 11px; font-weight: 500; padding: 0 8px; background: transparent; }"));

    _fullScreenBtn = new QPushButton(_controlBar);
    _fullScreenBtn->setObjectName(QStringLiteral("previewFullScreen"));
    _fullScreenBtn->setIcon(playerIcon(UDriveIcons::Symbol::FullScreen));
    _fullScreenBtn->setIconSize(QSize(18, 18));
    _fullScreenBtn->setToolTip(tr("Full screen (F)"));
    _fullScreenBtn->setAccessibleName(tr("Full screen"));
    _fullScreenBtn->setFixedSize(30, 30);
    _fullScreenBtn->setCursor(Qt::PointingHandCursor);
    _fullScreenBtn->setStyleSheet(btnStyle);

    controls->addWidget(_playBtn);
    controls->addWidget(rewind10);
    controls->addWidget(forward10);
    controls->addWidget(_timeLabel);
    controls->addStretch(1);
    controls->addWidget(_fullScreenBtn);
    barLayout->addLayout(controls);
    videoLayout->addWidget(_controlBar);

    _content->addWidget(_video);
    _player = new QMediaPlayer(this);
    _player->setObjectName(QStringLiteral("previewPlayer"));
    _player->setAudioOutput(new QAudioOutput(_player));
    _player->setVideoOutput(static_cast<QVideoWidget *>(_screen));

    // Fullscreen must include the controls, not just QVideoWidget's surface.
    _fullScreenWindow = new QWidget(this, Qt::Window);
    _fullScreenWindow->setObjectName(QStringLiteral("previewFullScreenWindow"));
    _fullScreenWindow->setWindowTitle(tr("Video preview"));
    _fullScreenWindow->setAttribute(Qt::WA_QuitOnClose, false);
    _fullScreenWindow->setStyleSheet(QStringLiteral("QWidget#previewFullScreenWindow { background: black; }"));
    _fullScreenWindow->installEventFilter(this);
    auto *fullScreenLayout = new QVBoxLayout(_fullScreenWindow);
    fullScreenLayout->setContentsMargins(0, 0, 0, 0);
    fullScreenLayout->setSpacing(0);

    // Scope shortcuts to each player window so focused playback controls also
    // support them, without intercepting keys elsewhere in the file browser.
    for (auto *shortcutParent : {_video, _fullScreenWindow}) {
        auto addShortcut = [this, shortcutParent](Qt::Key key, auto action) {
            auto *shortcut = new QShortcut(QKeySequence(key), shortcutParent);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this, action);
        };
        for (auto key : {Qt::Key_Space, Qt::Key_K}) addShortcut(key, [this] {
            if (_player->playbackState() == QMediaPlayer::PlayingState) _player->pause(); else _player->play();
        });
        for (auto key : {Qt::Key_Left, Qt::Key_J}) addShortcut(key, [this] {
            _player->setPosition(qMax(0LL, _player->position() - 10000));
        });
        for (auto key : {Qt::Key_Right, Qt::Key_L}) addShortcut(key, [this] {
            _player->setPosition(qMin(_player->duration(), _player->position() + 10000));
        });
        addShortcut(Qt::Key_F, [this] { setVideoFullScreen(!_videoFullScreen); });
        addShortcut(Qt::Key_Escape, [this] { setVideoFullScreen(false); });
    }

    connect(rewind10, &QPushButton::clicked, this, [this] {
        if (_player) _player->setPosition(qMax(0LL, _player->position() - 10000));
    });
    connect(forward10, &QPushButton::clicked, this, [this] {
        if (_player) _player->setPosition(qMin(_player->duration(), _player->position() + 10000));
    });
    connect(_playBtn, &QPushButton::clicked, this, [this] {
        if (_player->playbackState() == QMediaPlayer::PlayingState) _player->pause(); else _player->play();
    });
    connect(_fullScreenBtn, &QPushButton::clicked, this, [this] {
        setVideoFullScreen(!_videoFullScreen);
    });
    connect(_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (_playBtn) {
            if (state == QMediaPlayer::PlayingState) {
                _playBtn->setIcon(playerIcon(UDriveIcons::Symbol::Pause));
                _playBtn->setToolTip(tr("Pause (Space)"));
                _playBtn->setAccessibleName(tr("Pause"));
            } else {
                _playBtn->setIcon(playerIcon(UDriveIcons::Symbol::Play));
                _playBtn->setToolTip(tr("Play (Space)"));
                _playBtn->setAccessibleName(tr("Play"));
            }
        }
    });
    connect(_seek, &QSlider::sliderMoved, this, [this](int position) {
        if (_player->isSeekable()) {
            const qint64 targetMs = _player->duration() * position / 1000;
            _player->setPosition(targetMs);
            if (_timeLabel)
                _timeLabel->setText(formatMediaTime(targetMs) + QStringLiteral(" / ") + formatMediaTime(_player->duration()));
        }
    });
    connect(_player, &QMediaPlayer::positionChanged, this, [this](qint64 position) {
        if (_seek && !_seek->isSliderDown() && _player->duration() > 0)
            _seek->setValue(int(position * 1000 / _player->duration()));
        if (_timeLabel)
            _timeLabel->setText(formatMediaTime(position) + QStringLiteral(" / ") + formatMediaTime(_player->duration()));
        if (position > 0) markMediaReady();
    });
    connect(_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (_timeLabel)
            _timeLabel->setText(formatMediaTime(_player->position()) + QStringLiteral(" / ") + formatMediaTime(duration));
        if (duration > 0) markMediaReady();
    });
    connect(_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        // Some native backends transition straight to buffering/buffered media.
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferingMedia
            || status == QMediaPlayer::BufferedMedia || status == QMediaPlayer::EndOfMedia)
            markMediaReady();
    });
    connect(static_cast<QVideoWidget *>(_screen)->videoSink(), &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (frame.isValid()) markMediaReady();
    });
    connect(_player, &QMediaPlayer::errorOccurred, this, [this] {
        if (_content->currentWidget() != _video || _player->source().isEmpty()) return;
        const auto kind = kindForName(_title->text());
        _status->setText(kind == Kind::Audio
            ? tr("This audio file cannot be played with the available codecs. You can still download it.")
            : tr("This video cannot be played with the available codecs. You can still download it."));
    });
#endif
    connect(back, &QPushButton::clicked, this, &UDriveFilePreview::backRequested);
    connect(download, &QPushButton::clicked, this, &UDriveFilePreview::downloadRequested);
    connect(_zoom, &QComboBox::currentIndexChanged, this, &UDriveFilePreview::updateImage);
    clear();
}

UDriveFilePreview::~UDriveFilePreview()
{
#ifdef UDRIVE_HAS_MULTIMEDIA
    setVideoFullScreen(false);
#endif
}

#ifdef UDRIVE_HAS_MULTIMEDIA
void UDriveFilePreview::markMediaReady()
{
    if (_mediaReady || _content->currentWidget() != _video || _player->source().isEmpty() || _player->error() != QMediaPlayer::NoError) return;
    _mediaReady = true;
    const auto kind = kindForName(_title->text());
    _status->setText(kind == Kind::Audio ? tr("Audio preview") : tr("Video preview"));
    Q_EMIT ready();
}

void UDriveFilePreview::setVideoFullScreen(bool fullScreen)
{
    if (_videoFullScreen == fullScreen) return;
    if (fullScreen && (_content->currentWidget() != _video || !_screen->isVisible())) return;
    _videoFullScreen = fullScreen;
    if (fullScreen) {
        _fullScreenWindow->setScreen(_screen->screen());
        _fullScreenWindow->layout()->addWidget(_screen);
        _fullScreenWindow->layout()->addWidget(_controlBar);
        static_cast<QVBoxLayout *>(_fullScreenWindow->layout())->setStretch(0, 1);
        _fullScreenWindow->showFullScreen();
        _fullScreenWindow->raise();
        _fullScreenWindow->activateWindow();
    } else {
        auto *layout = static_cast<QVBoxLayout *>(_video->layout());
        layout->addWidget(_screen, 1);
        layout->addWidget(_controlBar);
        _fullScreenWindow->hide();
    }
    _screen->show();
    _controlBar->show();
    _fullScreenBtn->setIcon(playerIcon(fullScreen ? UDriveIcons::Symbol::ExitFullScreen : UDriveIcons::Symbol::FullScreen));
    _fullScreenBtn->setToolTip(fullScreen ? tr("Exit full screen (Esc / F)") : tr("Full screen (F)"));
    _fullScreenBtn->setAccessibleName(fullScreen ? tr("Exit full screen") : tr("Full screen"));
    _screen->setFocus();
}
#endif

void UDriveFilePreview::clear()
{
    ++_generation;
#ifdef UDRIVE_HAS_MULTIMEDIA
    setVideoFullScreen(false);
    _player->stop();
    _player->setSource({});
    _mediaReady = false;
    if (_screen) {
        _screen->show();
    }
    if (_playBtn) {
        _playBtn->setIcon(playerIcon(UDriveIcons::Symbol::Play));
        _playBtn->setToolTip(tr("Play (Space)"));
        _playBtn->setAccessibleName(tr("Play"));
    }
    if (_fullScreenBtn) {
        _fullScreenBtn->setIcon(playerIcon(UDriveIcons::Symbol::FullScreen));
        _fullScreenBtn->setToolTip(tr("Full screen (F)"));
        _fullScreenBtn->setAccessibleName(tr("Full screen"));
        _fullScreenBtn->show();
    }
    if (_timeLabel) {
        _timeLabel->setText(QStringLiteral("00:00 / 00:00"));
    }
    if (_seek) {
        _seek->setValue(0);
    }
#endif
    _storage.reset();
    _decoded = {};
    _image->clear();
    _image->resize(0, 0);
    _text->clear();
    _title->clear();
    _status->clear();
    _zoom->hide();
    _content->setCurrentIndex(0);
}

void UDriveFilePreview::showLoading(const QString &name)
{
    clear();
    _title->setText(name);
    _status->setText(tr("Loading preview…"));
}

void UDriveFilePreview::showError(const QString &message)
{
    _status->setText(message);
}

void UDriveFilePreview::openFile(const QString &path, const QString &name, std::shared_ptr<QTemporaryDir> storage)
{
    showLoading(name);
    _storage = std::move(storage);
    const auto kind = kindForName(name);
    if (!QFileInfo(path).isFile() || QFileInfo(path).size() > maximumBytes(kind)) {
        showError(tr("This file is too large to preview. You can still download it.")); return;
    }
    if (kind == Kind::Image) {
        const auto generation = _generation;
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, generation] {
            const QImage image = watcher->result();
            watcher->deleteLater();
            if (generation != _generation) return;
            if (image.isNull()) { showError(tr("This image could not be previewed. You can still download it.")); return; }
            _decoded = image;
            _image->setPixmap(QPixmap::fromImage(image));
            _zoom->setCurrentIndex(0);
            _zoom->show();
            _content->setCurrentWidget(_scroll);
            updateImage();
            _status->setText(tr("Image preview"));
            Q_EMIT ready();
        });
        watcher->setFuture(QtConcurrent::run([path, storage = _storage] {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            reader.setDecideFormatFromContent(true);
            if (!QList<QByteArray>{"png", "jpeg", "jpg", "gif", "webp", "bmp"}.contains(reader.format().toLower())) return QImage();
            const QSize size = reader.size();
            if (size.isEmpty() || size.width() > 32768 || size.height() > 32768 || qint64(size.width()) * size.height() > 64000000) return QImage();
            if (size.width() > 4096 || size.height() > 4096) reader.setScaledSize(size.scaled(4096, 4096, Qt::KeepAspectRatio));
            return reader.read();
        }));
    } else if (kind == Kind::Text) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { showError(tr("This file could not be previewed.")); return; }
        const QByteArray bytes = file.read(maximumBytes(kind) + 1);
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString text = decoder(bytes);
        if (bytes.size() > maximumBytes(kind) || bytes.contains('\0') || decoder.hasError()) {
            showError(tr("This file is not readable UTF-8 text. You can still download it.")); return;
        }
        _text->setPlainText(text);
        _content->setCurrentWidget(_text);
        _status->setText(tr("Read-only text preview"));
        Q_EMIT ready();
    } else if (kind == Kind::Video || kind == Kind::Audio) {
#ifdef UDRIVE_HAS_MULTIMEDIA
        _content->setCurrentWidget(_video);
        if (_screen) _screen->setVisible(kind == Kind::Video);
        if (_fullScreenBtn) _fullScreenBtn->setVisible(kind == Kind::Video);
        if (_controlBar) {
            _controlBar->setStyleSheet(kind == Kind::Audio
                ? QStringLiteral("QWidget#previewControlBar { background-color: #1a1a1a; border-radius: 8px; border: 1px solid rgba(255, 255, 255, 0.15); }")
                : QStringLiteral("QWidget#previewControlBar { background-color: #0f0f0f; border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; border-top: 1px solid rgba(255, 255, 255, 0.08); }"));
        }
        _player->setSource(QUrl::fromLocalFile(path));
        _player->play();
#else
        showError(kind == Kind::Audio
            ? tr("Audio preview is not included in this build. You can still download this file.")
            : tr("Video preview is not included in this build. You can still download this file."));
#endif
    } else {
        showError(tr("Preview is not available for this file type. Use Download to save a copy."));
    }
}

void UDriveFilePreview::updateImage()
{
    if (_decoded.isNull()) return;
    const int zoom = _zoom->currentData().toInt();
    const QSize size = zoom ? _decoded.size() * (zoom / 100.0)
        : _decoded.size().scaled(_scroll->viewport()->size().boundedTo(_decoded.size()), Qt::KeepAspectRatio);
    _image->resize(size.expandedTo(QSize(1, 1)));
}

void UDriveFilePreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImage();
}

bool UDriveFilePreview::eventFilter(QObject *watched, QEvent *event)
{
#ifdef UDRIVE_HAS_MULTIMEDIA
    if (watched == _fullScreenWindow) {
        if (event->type() == QEvent::Close) {
            setVideoFullScreen(false);
            event->ignore();
            return true;
        }
        if (event->type() == QEvent::WindowStateChange && _videoFullScreen && !_fullScreenWindow->isFullScreen())
            setVideoFullScreen(false);
    }
    if (watched == _screen && event->type() == QEvent::MouseButtonDblClick) {
        setVideoFullScreen(!_videoFullScreen);
        return true;
    }
    if (watched == _seek && event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton && _seek) {
            const int val = QStyle::sliderValueFromPosition(_seek->minimum(), _seek->maximum(), mouseEvent->pos().x(), _seek->width());
            _seek->setValue(val);
            if (_player && _player->isSeekable() && _player->duration() > 0) {
                const qint64 targetMs = _player->duration() * val / 1000;
                _player->setPosition(targetMs);
                if (_timeLabel)
                    _timeLabel->setText(formatMediaTime(targetMs) + QStringLiteral(" / ") + formatMediaTime(_player->duration()));
            }
            return true;
        }
    }
#endif
    return QWidget::eventFilter(watched, event);
}
}
