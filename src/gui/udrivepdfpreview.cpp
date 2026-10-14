// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivepdfpreview.h"
#include "udriveicons.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>
#include <algorithm>
#include <cmath>

namespace OCC {
UDrivePdfPreview::UDrivePdfPreview(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("drivePdfPreview"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    auto *header = new QHBoxLayout;
    auto *back = new QPushButton(tr("Back to files"), this);
    back->setObjectName(QStringLiteral("pdfBack"));
    back->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    back->setIconSize(QSize(20, 20));
    header->addWidget(back);
    _title = new QLabel(this);
    _title->setTextFormat(Qt::PlainText);
    _title->setWordWrap(true);
    header->addWidget(_title, 1);
    layout->addLayout(header);
    auto *toolbar = new QHBoxLayout;
    _previous = new QPushButton(tr("Previous"), this);
    _previous->setObjectName(QStringLiteral("pdfPrevious"));
    _previous->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    _previous->setIconSize(QSize(20, 20));
    _next = new QPushButton(tr("Next"), this);
    _next->setObjectName(QStringLiteral("pdfNext"));
    _next->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Next));
    _next->setIconSize(QSize(20, 20));
    _page = new QSpinBox(this);
    _page->setObjectName(QStringLiteral("pdfPage"));
    _page->setAccessibleName(tr("PDF page number"));
    _page->setKeyboardTracking(false);
    _count = new QLabel(this);
    toolbar->addWidget(_previous);
    toolbar->addWidget(_page);
    toolbar->addWidget(_count);
    toolbar->addWidget(_next);
    toolbar->addStretch();
    _zoom = new QComboBox(this);
    _zoom->setObjectName(QStringLiteral("pdfZoom"));
    _zoom->setProperty("udriveCombo", true);
    _zoom->setAccessibleName(tr("PDF zoom"));
    _zoom->addItem(tr("Fit width"), -1);
    _zoom->addItem(tr("Fit page"), -2);
    for (int value : {50, 75, 100, 125, 150, 200, 300}) _zoom->addItem(tr("%1%").arg(value), value);
    _zoom->setCurrentIndex(1);
    toolbar->addWidget(_zoom);
    layout->addLayout(toolbar);
    _status = new QLabel(this);
    _status->setObjectName(QStringLiteral("pdfStatus"));
    _status->setTextFormat(Qt::PlainText);
    _status->setWordWrap(true);
    layout->addWidget(_status);
    _scroll = new QScrollArea(this);
    _scroll->setAlignment(Qt::AlignCenter);
    _image = new QLabel;
    _image->setObjectName(QStringLiteral("pdfPageImage"));
    _image->setAccessibleName(tr("PDF page preview"));
    _image->setScaledContents(true);
    _scroll->setWidget(_image);
    layout->addWidget(_scroll, 1);
    _renderTimer = new QTimer(this);
    _renderTimer->setSingleShot(true);
    _renderTimer->setTimerType(Qt::PreciseTimer);
    _renderTimer->setInterval(RenderTimeoutMs);
    connect(_renderTimer, &QTimer::timeout, this, [this] { failRender(tr("This PDF took too long to render. Download it to open it on your computer.")); });
    _resizeTimer = new QTimer(this);
    _resizeTimer->setSingleShot(true);
    _resizeTimer->setInterval(100);
    connect(back, &QPushButton::clicked, this, &UDrivePdfPreview::backRequested);
    connect(_previous, &QPushButton::clicked, this, [this] { _page->setValue(_page->value() - 1); });
    connect(_next, &QPushButton::clicked, this, [this] { _page->setValue(_page->value() + 1); });
    connect(_page, &QSpinBox::valueChanged, this, [this] { updateControls(); scheduleRender(); });
    connect(_zoom, &QComboBox::currentIndexChanged, this, &UDrivePdfPreview::scheduleRender);
    connect(_resizeTimer, &QTimer::timeout, this, &UDrivePdfPreview::scheduleRender);
    clear();
}

UDrivePdfPreview::~UDrivePdfPreview()
{
    stopRenderer();
}

void UDrivePdfPreview::stopRenderer()
{
    _renderTimer->stop();
    if (auto *process = _process.data()) {
        _process.clear();
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->setObjectName(QString());
        process->deleteLater(); // Never delete a QProcess from its own signal handler.
    }
    _busy = false;
    _output.clear();
    _frame = {};
    _expectedBytes = -1;
}

void UDrivePdfPreview::clear()
{
    ++_generation;
    _pending = false;
    stopRenderer();
    _failed = false;
    _resizeTimer->stop();
    _path.clear(); _pageCount = 0; _storage.reset();
    _title->clear(); _status->clear(); _image->clear(); _image->resize(0, 0);
    const QSignalBlocker blocker(_page);
    _page->setRange(1, 1); _page->setValue(1); _count->clear();
    updateControls();
}

void UDrivePdfPreview::openFile(const QString &path, const QString &name, std::shared_ptr<QTemporaryDir> storage)
{
    clear();
    _path = path; _storage = std::move(storage);
    _title->setText(name);
    _status->setText(tr("Loading PDF…"));
    layout()->activate();
    scheduleRender();
}

void UDrivePdfPreview::updateControls()
{
    const bool loaded = _pageCount > 0 && !_failed;
    _previous->setEnabled(loaded && _page->value() > 1);
    _next->setEnabled(loaded && _page->value() < _pageCount);
    _page->setEnabled(loaded); _zoom->setEnabled(loaded);
}

void UDrivePdfPreview::failRender(const QString &error)
{
    stopRenderer();
    if (_storage) { _storage->remove(); _storage.reset(); }
    _pending = false;
    _failed = true;
    _image->clear();
    _status->setText(error);
    updateControls();
    Q_EMIT previewFailed(error);
}

void UDrivePdfPreview::scheduleRender()
{
    if (_path.isEmpty() || _failed) return;
    ++_generation;
    _pending = true;
    _image->clear();
    _status->setText(tr("Loading PDF…"));
    if (!_busy) render();
}

void UDrivePdfPreview::render()
{
    _pending = false;
    _busy = true;
    _renderGeneration = _generation;
    _renderPage = _page->value();
    _renderDpr = devicePixelRatioF();
    _output.clear(); _frame = {}; _expectedBytes = -1;
    const QSize viewport = _scroll->viewport()->size() - QSize(24, 24);
    const QByteArray request = QJsonDocument(QJsonObject{{QStringLiteral("page"), _renderPage},
        {QStringLiteral("zoom"), _zoom->currentData().toInt()}, {QStringLiteral("width"), std::clamp(viewport.width(), 1, 32768)},
        {QStringLiteral("height"), std::clamp(viewport.height(), 1, 32768)}, {QStringLiteral("dpr"), _renderDpr}}).toJson(QJsonDocument::Compact) + '\n';
    _renderTimer->start();
    if (_process && _process->state() == QProcess::Running) {
        _process->write(request);
        return;
    }
    auto *process = new QProcess(this);
    _process = process;
    process->setObjectName(QStringLiteral("pdfRendererProcess"));
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setStandardErrorFile(QProcess::nullDevice());
    connect(process, &QProcess::readyReadStandardOutput, this, &UDrivePdfPreview::readRenderResult);
    connect(process, &QProcess::started, this, [this, process, request] {
        if (_process == process) process->write(request);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process] {
        if (_process == process) failRender(tr("The PDF renderer could not run. Download this file to open it on your computer."));
    });
    connect(process, &QProcess::finished, this, [this, process] {
        if (_process == process) failRender(tr("The PDF renderer could not run. Download this file to open it on your computer."));
    });
    // Use only the trusted sibling helper, never PATH or the normal app startup.
    process->start(QCoreApplication::applicationDirPath() + QStringLiteral("/udrive-pdf-renderer"), {_path});
}

void UDrivePdfPreview::readRenderResult()
{
    if (!_process || !_busy) return;
    const auto completedGeneration = _renderGeneration;
    const QPointer<UDrivePdfPreview> guard(this);
    constexpr qint64 MaximumReplyBytes = 4096LL * 4096 * 4 + 4097;
    _output += _process->read(MaximumReplyBytes + 1 - _output.size());
    auto invalid = [this] { failRender(tr("The PDF renderer returned an invalid page.")); };
    if (_output.size() > MaximumReplyBytes || _process->bytesAvailable() > 0) { invalid(); return; }
    if (_expectedBytes < 0) {
        const auto newline = _output.indexOf('\n');
        if (newline < 0) { if (_output.size() > 4096) invalid(); return; }
        if (newline > 4096) { invalid(); return; }
        const auto document = QJsonDocument::fromJson(_output.left(newline));
        if (!document.isObject()) { invalid(); return; }
        _frame = document.object();
        _expectedBytes = _frame.value(QStringLiteral("bytes")).toInteger(-1);
        if (_expectedBytes < 0 || _expectedBytes > 4096LL * 4096 * 4 || !_frame.value(QStringLiteral("error")).isString()) { invalid(); return; }
        _output.remove(0, newline + 1);
    }
    if (_output.size() < _expectedBytes) return;
    if (_output.size() != _expectedBytes) { invalid(); return; }
    _renderTimer->stop();
    _busy = false;
    const QString error = _frame.value(QStringLiteral("error")).toString();
    const int pages = _frame.value(QStringLiteral("pages")).toInt();
    if (_renderGeneration == _generation && !_path.isEmpty()) {
        if (!error.isEmpty()) {
            if (_expectedBytes != 0 || pages < 0) { invalid(); return; }
            const QByteArray source = error.toUtf8();
            _status->setText(_frame.value(QStringLiteral("documentError")).toBool()
                ? QCoreApplication::translate("QObject", source.constData()) : tr(source.constData()));
            if (pages == 0) { failRender(_status->text()); return; }
            _pageCount = pages;
            updateControls();
            Q_EMIT previewFailed(_status->text());
            if (!guard || _generation != completedGeneration) return;
        } else {
            const int width = _frame.value(QStringLiteral("width")).toInt();
            const int height = _frame.value(QStringLiteral("height")).toInt();
            const QSize display(_frame.value(QStringLiteral("displayWidth")).toInt(), _frame.value(QStringLiteral("displayHeight")).toInt());
            if (pages < _renderPage || width < 1 || height < 1 || width > 4096 || height > 4096
                || _expectedBytes != qint64(width) * height * 4 || display.isEmpty() || display.width() > 32768 || display.height() > 32768) { invalid(); return; }
            _pageCount = pages;
            const QSignalBlocker blocker(_page);
            _page->setRange(1, pages);
            _count->setText(tr("of %1").arg(pages));
            layout()->activate();
            const QImage image(reinterpret_cast<const uchar *>(_output.constData()), width, height, width * 4, QImage::Format_RGBA8888);
            auto pixmap = QPixmap::fromImage(image.copy());
            if (pixmap.isNull()) { invalid(); return; }
            pixmap.setDevicePixelRatio(_renderDpr);
            _image->setPixmap(pixmap); _image->resize(display);
            _scroll->verticalScrollBar()->setValue(0);
            _status->setText(tr("Page %1 of %2").arg(_renderPage).arg(pages));
            _image->setAccessibleDescription(_status->text());
            updateControls();
            Q_EMIT pageRendered(_renderPage);
            if (!guard || _generation != completedGeneration) return;
        }
        updateControls();
    }
    _output.clear();
    if (_pending && !_path.isEmpty()) render();
}

void UDrivePdfPreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!_path.isEmpty() && _zoom->currentData().toInt() < 0) _resizeTimer->start();
}
}
