// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivepdfpreview.h"
#include "udrivecombobox.h"
#include "udriveicons.h"
#include <QComboBox>
#include <QAbstractItemView>
#include <QLabel>
#include <QPushButton>
#include <QAbstractScrollArea>
#include <QPainter>
#include <functional>
#include <limits>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QProcess>
#include <QResizeEvent>
#include <algorithm>
#include <cmath>

namespace OCC {
namespace {
constexpr qint64 MaximumCachedBytes = 128LL * 1024 * 1024;
constexpr int MaximumCachedPages = 64;
qint64 rasterBytes(const QPixmap &pixmap)
{
    return qint64(pixmap.width()) * pixmap.height() * 4;
}
}
// Keep long filenames on one line while retaining their full accessible name.
class PdfTitleLabel : public QLabel
{
public:
    explicit PdfTitleLabel(QWidget *parent) : QLabel(parent)
    {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(60);
    }
    void setText(const QString &text)
    {
        _name = text;
        setAccessibleName(text);
        setToolTip(QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped()));
        elide();
    }
    void clear() { setText({}); }
protected:
    void resizeEvent(QResizeEvent *event) override { QLabel::resizeEvent(event); elide(); }
private:
    void elide() { QLabel::setText(fontMetrics().elidedText(_name, Qt::ElideMiddle, width())); }
    QString _name;
};

// Paint only the visible portion of a virtual document. No widget or raster is
// allocated for every page, and document height is independent of QWidget limits.
class PdfPageArea : public QAbstractScrollArea
{
public:
    using QAbstractScrollArea::QAbstractScrollArea;
    std::function<void(QPainter &)> paint;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(viewport());
        if (paint) paint(painter);
    }
    void scrollContentsBy(int, int) override { viewport()->update(); }
};

UDrivePdfPreview::UDrivePdfPreview(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("drivePdfPreview"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    _toolbar = new QWidget(this);
    _toolbar->setObjectName(QStringLiteral("pdfToolbar"));
    _toolbar->setFixedHeight(64);
    auto *header = new QHBoxLayout(_toolbar);
    header->setContentsMargins(14, 12, 14, 12);
    header->setSpacing(12);
    auto *back = new QPushButton(_toolbar);
    back->setObjectName(QStringLiteral("pdfBack"));
    back->setToolTip(tr("Back to files"));
    back->setAccessibleName(tr("Back to files"));
    back->setFixedSize(34, 34);
    back->setCursor(Qt::PointingHandCursor);
    back->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    back->setIconSize(QSize(20, 20));
    header->addWidget(back);
    auto *fileIcon = new QLabel(_toolbar);
    fileIcon->setPixmap(UDriveIcons::icon(UDriveIcons::Symbol::Pdf, false, QColor("#E77373")).pixmap(QSize(24, 24)));
    fileIcon->setFixedSize(24, 24);
    header->addWidget(fileIcon);
    _title = new PdfTitleLabel(_toolbar);
    _title->setObjectName(QStringLiteral("pdfTitle"));
    header->addWidget(_title, 1);
    auto *navigation = new QWidget(_toolbar);
    navigation->setObjectName(QStringLiteral("pdfPageNavigation"));
    navigation->setFixedHeight(36);
    auto *pager = new QHBoxLayout(navigation);
    pager->setContentsMargins(3, 3, 3, 3);
    pager->setSpacing(4);
    _previous = new QPushButton(navigation);
    _previous->setObjectName(QStringLiteral("pdfPrevious"));
    _previous->setToolTip(tr("Previous page"));
    _previous->setAccessibleName(tr("Previous page"));
    _previous->setFixedSize(28, 28);
    _previous->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Back));
    _previous->setIconSize(QSize(16, 16));
    _next = new QPushButton(navigation);
    _next->setObjectName(QStringLiteral("pdfNext"));
    _next->setToolTip(tr("Next page"));
    _next->setAccessibleName(tr("Next page"));
    _next->setFixedSize(28, 28);
    _next->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Next));
    _next->setIconSize(QSize(16, 16));
    _page = new QSpinBox(navigation);
    _page->setObjectName(QStringLiteral("pdfPage"));
    _page->setAccessibleName(tr("PDF page number"));
    _page->setKeyboardTracking(false);
    _page->setButtonSymbols(QAbstractSpinBox::NoButtons);
    _page->setAlignment(Qt::AlignCenter);
    _page->setFixedSize(38, 28);
    _count = new QLabel(navigation);
    _count->setObjectName(QStringLiteral("pdfPageCount"));
    pager->addWidget(_previous);
    pager->addWidget(_page);
    pager->addWidget(_count);
    pager->addWidget(_next);
    header->addWidget(navigation);
    _zoom = new UDriveComboBox(_toolbar);
    _zoom->setObjectName(QStringLiteral("pdfZoom"));
    _zoom->setProperty("udriveCombo", true);
    _zoom->view()->setFrameShape(QFrame::NoFrame);
    _zoom->setAccessibleName(tr("PDF zoom"));
    _zoom->setFixedSize(138, 36);
    _zoom->addItem(tr("Fit width"), -1);
    _zoom->addItem(tr("Fit page"), -2);
    for (int value : {50, 75, 100, 125, 150, 200, 300}) _zoom->addItem(tr("%1%").arg(value), value);
    _zoom->setCurrentIndex(0);
    header->addWidget(_zoom);
    auto *download = new QPushButton(_toolbar);
    download->setObjectName(QStringLiteral("pdfDownload"));
    download->setToolTip(tr("Download"));
    download->setAccessibleName(tr("Download"));
    download->setIcon(UDriveIcons::icon(UDriveIcons::Symbol::Download));
    download->setIconSize(QSize(18, 18));
    download->setFixedSize(34, 34);
    header->addWidget(download);
    layout->addWidget(_toolbar);
    auto *pageArea = new PdfPageArea(this);
    _scroll = pageArea;
    pageArea->paint = [this](QPainter &painter) { paintPages(painter); };
    _scroll->setObjectName(QStringLiteral("pdfCanvas"));
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    _scroll->viewport()->setObjectName(QStringLiteral("pdfViewport"));
    _scroll->viewport()->installEventFilter(this);
    _image = new QLabel(_scroll->viewport());
    _image->setObjectName(QStringLiteral("pdfPageImage"));
    _image->setAccessibleName(tr("PDF page preview"));
    _image->setScaledContents(true);
    _image->setAttribute(Qt::WA_TransparentForMouseEvents);
    _scroll->viewport()->setAccessibleName(tr("PDF document pages"));
    _status = new QLabel(_scroll->viewport());
    _status->setObjectName(QStringLiteral("pdfStatus"));
    _status->setTextFormat(Qt::PlainText);
    _status->setWordWrap(true);
    _status->setAlignment(Qt::AlignCenter);
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
    connect(download, &QPushButton::clicked, this, &UDrivePdfPreview::downloadRequested);
    connect(_previous, &QPushButton::clicked, this, [this] { _page->setValue(_page->value() - 1); });
    connect(_next, &QPushButton::clicked, this, [this] { _page->setValue(_page->value() + 1); });
    connect(_page, &QSpinBox::valueChanged, this, &UDrivePdfPreview::navigateToPage);
    connect(_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!_layoutPending && _pageCount > 0) {
            const QSignalBlocker blocker(_page);
            _page->setValue(pageAt(scrollOffset() + _scroll->viewport()->height() / 2));
        }
        updateViewport();
    });
    connect(_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateViewport(); });
    connect(_zoom, &QComboBox::currentIndexChanged, this, &UDrivePdfPreview::scheduleRender);
    connect(_resizeTimer, &QTimer::timeout, this, [this] {
        if (_path.isEmpty() || _failed) return;
        if (_zoom->currentData().toInt() < 0) scheduleRender();
        else if (!_layoutPending) {
            const int anchor = _page->value();
            updateScrollRange(anchor, scrollOffset() - pageTop(anchor));
            updateViewport();
        }
    });
    updateAppearance();
    // A styled child can retain a resolved palette, so observe application
    // palette changes even when an ancestor has an explicit palette.
    qGuiApp->installEventFilter(this);
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
    stopRenderer();
    _failed = false;
    _resizeTimer->stop();
    _path.clear(); _pageCount = 0; _storage.reset();
    _pageSizes.clear(); _pagePixmaps.clear(); _pageErrors.clear(); _recentPages.clear();
    _defaultPageSize = {}; _layoutPending = true; _announcedPage = 0;
    _scroll->verticalScrollBar()->setRange(0, 0);
    _scroll->horizontalScrollBar()->setRange(0, 0);
    _scroll->viewport()->update();
    _title->clear(); _status->clear(); _image->clear(); _image->resize(0, 0);
    _status->hide();
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
    _count->setText(loaded ? tr("/ %1").arg(_pageCount) : QStringLiteral("/ —"));
    _page->setFixedWidth(std::max(38, _page->fontMetrics().horizontalAdvance(QString::number(_pageCount)) + 18));
}

void UDrivePdfPreview::failRender(const QString &error)
{
    stopRenderer();
    if (_storage) { _storage->remove(); _storage.reset(); }
    _failed = true;
    _pagePixmaps.clear(); _recentPages.clear();
    _image->clear();
    _scroll->viewport()->update();
    _status->setText(error);
    _status->show();
    positionStatus();
    updateControls();
    Q_EMIT previewFailed(error);
}

void UDrivePdfPreview::scheduleRender()
{
    if (_path.isEmpty() || _failed) return;
    ++_generation;
    _layoutPending = true;
    _announcedPage = 0;
    _pagePixmaps.clear(); _pageErrors.clear(); _recentPages.clear();
    _image->clear();
    _status->setText(tr("Loading PDF…"));
    _status->show();
    positionStatus();
    _scroll->viewport()->update();
    render();
}

void UDrivePdfPreview::render()
{
    if (_busy || _path.isEmpty() || _failed) return;
    const auto needed = neededPages();
    const auto missing = std::find_if(needed.begin(), needed.end(), [this](int page) {
        return !_pagePixmaps.contains(page) && !_pageErrors.contains(page);
    });
    if (missing == needed.end()) return;
    _busy = true;
    _renderGeneration = _generation;
    _renderPage = *missing;
    _renderDpr = devicePixelRatioF();
    _output.clear(); _frame = {}; _expectedBytes = -1;
    const QSize viewport = _scroll->viewport()->size() - QSize(48, 48);
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
    QString renderer = QCoreApplication::applicationDirPath() + QStringLiteral("/udrive-pdf-renderer");
#ifdef Q_OS_WIN
    renderer += QStringLiteral(".exe");
#endif
    process->start(renderer, {_path});
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
        _output.reserve(_expectedBytes);
    }
    _renderTimer->start();
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
            const QString message = _frame.value(QStringLiteral("documentError")).toBool()
                ? QCoreApplication::translate("QObject", source.constData()) : tr(source.constData());
            _status->setText(message);
            _status->show();
            positionStatus();
            if (pages == 0) { failRender(message); return; }
            _pageCount = pages;
            _pageErrors.insert(_renderPage, message);
            if (_defaultPageSize.isEmpty()) _defaultPageSize = QSize(600, 800);
            _layoutPending = false;
            {
                const QSignalBlocker blocker(_page);
                _page->setRange(1, pages);
            }
            updateScrollRange(_page->value());
            updateViewport();
            Q_EMIT previewFailed(message);
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
            layout()->activate();
            const QImage image(reinterpret_cast<const uchar *>(_output.constData()), width, height, width * 4, QImage::Format_RGBA8888);
            auto pixmap = QPixmap::fromImage(image.copy());
            if (pixmap.isNull()) { invalid(); return; }
            pixmap.setDevicePixelRatio(_renderDpr);
            const int anchor = _page->value();
            const qint64 offset = _layoutPending ? (anchor == 1 ? -24 : 0) : scrollOffset() - pageTop(anchor);
            if (_layoutPending) {
                _defaultPageSize = display;
                _pageSizes.clear();
                _layoutPending = false;
            }
            if (display != _defaultPageSize) _pageSizes.insert(_renderPage, display);
            // Keep a bounded cache, retaining visible pages before older pages.
            const auto needed = neededPages();
            qint64 cachedBytes = rasterBytes(pixmap);
            for (const auto &cached : _pagePixmaps) cachedBytes += rasterBytes(cached);
            while (!_pagePixmaps.isEmpty() && (_pagePixmaps.size() >= MaximumCachedPages || cachedBytes > MaximumCachedBytes)) {
                auto victim = std::find_if(_recentPages.begin(), _recentPages.end(),
                    [&needed](int page) { return !needed.contains(page); });
                if (victim == _recentPages.end()) victim = _recentPages.begin();
                cachedBytes -= rasterBytes(_pagePixmaps.take(*victim));
                _recentPages.erase(victim);
            }
            _pagePixmaps.insert(_renderPage, pixmap);
            _recentPages.removeAll(_renderPage);
            _recentPages.append(_renderPage);
            updateScrollRange(anchor, offset);
            updateViewport();
            if (!guard || _generation != completedGeneration) return;
        }
        updateControls();
    }
    _output.clear();
    render();
}

QSize UDrivePdfPreview::pageSize(int page) const
{
    return _pageSizes.value(page, _defaultPageSize);
}

qint64 UDrivePdfPreview::pageTop(int page) const
{
    qint64 top = 24 + qint64(page - 1) * (_defaultPageSize.height() + 24);
    for (auto it = _pageSizes.cbegin(); it != _pageSizes.cend() && it.key() < page; ++it)
        top += it.value().height() - _defaultPageSize.height();
    return top;
}

qint64 UDrivePdfPreview::scrollOffset() const
{
    return qRound64(_scroll->verticalScrollBar()->value() * _scrollScale);
}

int UDrivePdfPreview::pageAt(qint64 position) const
{
    int first = 1, last = std::max(1, _pageCount);
    while (first < last) {
        const int middle = first + int((qint64(last) - first + 1) / 2);
        if (pageTop(middle) <= position) first = middle;
        else last = middle - 1;
    }
    return first;
}

void UDrivePdfPreview::updateScrollRange(int anchorPage, qint64 anchorOffset)
{
    if (_pageCount < 1 || _defaultPageSize.isEmpty()) return;
    const QSignalBlocker verticalBlocker(_scroll->verticalScrollBar());
    const QSignalBlocker horizontalBlocker(_scroll->horizontalScrollBar());
    const qint64 height = pageTop(_pageCount) + pageSize(_pageCount).height() + 24;
    const qint64 extent = std::max<qint64>(0, height - _scroll->viewport()->height());
    // QScrollBar uses int positions; map exceptionally long documents without
    // allocating an enormous widget or overflowing the document coordinates.
    _scrollScale = std::max(1.0, double(extent) / std::numeric_limits<int>::max());
    auto *vertical = _scroll->verticalScrollBar();
    vertical->setRange(0, int(std::min<double>(extent / _scrollScale, std::numeric_limits<int>::max())));
    vertical->setSingleStep(std::max(1, int(48 / _scrollScale)));
    vertical->setPageStep(std::max(1, int(_scroll->viewport()->height() / _scrollScale)));
    vertical->setValue(int(std::min<double>(std::clamp<qint64>(pageTop(anchorPage) + anchorOffset, 0, extent) / _scrollScale, std::numeric_limits<int>::max())));
    int width = _defaultPageSize.width();
    for (const auto &size : _pageSizes) width = std::max(width, size.width());
    _scroll->horizontalScrollBar()->setRange(0, std::max(0, width + 48 - _scroll->viewport()->width()));
    _scroll->horizontalScrollBar()->setPageStep(_scroll->viewport()->width());
}

QList<int> UDrivePdfPreview::neededPages() const
{
    const int current = _page->value();
    QList<int> pages{current};
    if (_layoutPending || !_pageCount) return pages;
    const qint64 top = scrollOffset(), bottom = top + _scroll->viewport()->height();
    const auto estimatedBytes = [this](int page) {
        const QSize size = pageSize(page);
        const double scale = std::min(devicePixelRatioF(), 4096.0 / std::max(size.width(), size.height()));
        // Allow for rounding between logical and native page dimensions.
        return qint64(std::clamp(int(std::ceil(size.width() * scale)) + 2, 1, 4096))
            * std::clamp(int(std::ceil(size.height() * scale)) + 2, 1, 4096) * 4;
    };
    qint64 bytes = estimatedBytes(current);
    for (qint64 candidate = pageAt(top), checked = 0; candidate <= _pageCount && checked < 1000 && pages.size() < MaximumCachedPages; ++candidate, ++checked) {
        const int page = int(candidate);
        if (pageTop(page) >= bottom) break;
        if (page == current || pageTop(page) + pageSize(page).height() <= top) continue;
        const qint64 cost = estimatedBytes(page);
        if (bytes + cost > MaximumCachedBytes) continue;
        pages.append(page);
        bytes += cost;
    }
    return pages;
}

void UDrivePdfPreview::navigateToPage(int page)
{
    _announcedPage = 0;
    if (!_layoutPending && _pageCount) {
        const QSignalBlocker blocker(_scroll->verticalScrollBar());
        _scroll->verticalScrollBar()->setValue(int(std::min<double>(pageTop(page) / _scrollScale, std::numeric_limits<int>::max())));
    }
    updateViewport();
}

void UDrivePdfPreview::updateViewport()
{
    const int current = _page->value();
    const auto cached = _pagePixmaps.constFind(current);
    if (cached != _pagePixmaps.cend() && !_failed) {
        const QSize size = pageSize(current);
        const int x = std::max(24, (_scroll->viewport()->width() + _scroll->horizontalScrollBar()->maximum() - size.width()) / 2)
            - _scroll->horizontalScrollBar()->value();
        const qint64 y = pageTop(current) - scrollOffset();
        _image->setPixmap(cached.value());
        _image->resize(size);
        _image->move(x, int(std::clamp<qint64>(y, -32768, 32768)));
        _image->setVisible(y < _scroll->viewport()->height() && y + size.height() > 0);
        _image->setAccessibleDescription(tr("Page %1 of %2").arg(current).arg(_pageCount));
        _status->hide();
        _recentPages.removeAll(current);
        _recentPages.append(current);
        if (_announcedPage != current) {
            _announcedPage = current;
            const auto generation = _generation;
            // Defer notifications so a receiver may safely close or replace the
            // preview without invalidating a renderer or scroll event handler.
            QTimer::singleShot(0, this, [this, current, generation] {
                if (_generation == generation && _page->value() == current && _pagePixmaps.contains(current))
                    Q_EMIT pageRendered(current);
            });
        }
    } else {
        _image->clear();
        _image->hide();
        if (!_path.isEmpty() && !_failed) {
            _status->setText(_pageErrors.value(current, tr("Loading PDF…")));
            positionStatus();
            _status->show();
        }
    }
    updateControls();
    _scroll->viewport()->update();
    QTimer::singleShot(0, this, &UDrivePdfPreview::render);
}

void UDrivePdfPreview::paintPages(QPainter &painter)
{
    if (_path.isEmpty() || _failed || _pageCount < 1 || _defaultPageSize.isEmpty()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const qint64 offset = scrollOffset();
    for (qint64 candidate = pageAt(offset), drawn = 0; candidate <= _pageCount && drawn < 1000; ++drawn, ++candidate) {
        const int page = int(candidate);
        const qint64 y = pageTop(page) - offset;
        if (y >= _scroll->viewport()->height()) break;
        const QSize size = pageSize(page);
        if (y + size.height() <= 0) continue;
        if (page == _page->value() && _image->isVisible()) continue;
        const int x = std::max(24, (_scroll->viewport()->width() + _scroll->horizontalScrollBar()->maximum() - size.width()) / 2)
            - _scroll->horizontalScrollBar()->value();
        const QRect rect(QPoint(x, int(y)), size);
        painter.fillRect(rect, Qt::white);
        const auto cached = _pagePixmaps.constFind(page);
        if (cached != _pagePixmaps.cend()) painter.drawPixmap(rect, cached.value(), cached.value().rect());
        else {
            painter.setPen(QColor(QStringLiteral("#707070")));
            painter.drawText(rect, Qt::AlignCenter, _pageErrors.value(page, tr("Page %1").arg(page)));
        }
        painter.setPen(QColor(0, 0, 0, 30));
        painter.drawRect(rect.adjusted(0, 0, -1, -1));
    }
}

void UDrivePdfPreview::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!_path.isEmpty()) _resizeTimer->start();
}

void UDrivePdfPreview::positionStatus()
{
    if (!_status) return;
    const QSize viewport = _scroll->viewport()->size();
    const int width = std::max(1, viewport.width() - 48);
    const int height = std::max(48, _status->heightForWidth(width));
    _status->setGeometry(24, std::max(16, (viewport.height() - height) / 2), width, height);
    _status->raise();
}

bool UDrivePdfPreview::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange) {
        const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
        if (dark != _darkAppearance) updateAppearance();
    }
    if (_scroll && watched == _scroll->viewport() && event->type() == QEvent::Resize) {
        positionStatus();
        if (_resizeTimer && !_path.isEmpty() && !_failed) _resizeTimer->start();
    }
    return QWidget::eventFilter(watched, event);
}

void UDrivePdfPreview::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (_toolbar && _scroll && (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange))
        updateAppearance();
}

void UDrivePdfPreview::updateAppearance()
{
    if (_updatingAppearance) return;
    _updatingAppearance = true;
    const bool dark = QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
    _darkAppearance = dark;
    // Match the surfaces and controls used by the desktop workspace.
    const QString surface = dark ? QStringLiteral("#292929") : QStringLiteral("#FFFFFF");
    const QString text = dark ? QStringLiteral("#EDEEF0") : QStringLiteral("#2B3035");
    const QString border = dark ? QStringLiteral("#414141") : QStringLiteral("#E1E5E8");
    const QString hover = dark ? QStringLiteral("#323E35") : QStringLiteral("#EAF8EF");
    const QString muted = dark ? QStringLiteral("#939D97") : QStringLiteral("#7D8781");
    const QString canvas = dark ? surface : QStringLiteral("#F6F7F8");
    const QString field = dark ? QStringLiteral("#303330") : QStringLiteral("#FAFCFB");
    const QString arrow = dark ? QStringLiteral("chevron-dark") : QStringLiteral("chevron-light");
    setStyleSheet(QStringLiteral(
        "QWidget#drivePdfPreview { background: %1; border-radius: 15px; }"
        "QWidget#pdfToolbar { background: %1; border-bottom: 1px solid %3; border-top-left-radius: 15px; border-top-right-radius: 15px; }"
        "QLabel#pdfTitle { color: %2; font-size: 13px; font-weight: 600; }"
        "QWidget#drivePdfPreview QPushButton { min-width: 0; min-height: 0; padding: 0; border: none; border-radius: 6px; background: transparent; }"
        "QWidget#drivePdfPreview QPushButton:hover { background: %4; }"
        "QWidget#drivePdfPreview QPushButton:focus { border: 1px solid #40D979; }"
        "QWidget#drivePdfPreview QPushButton#pdfBack { border: none; background: transparent; }"
        "QWidget#drivePdfPreview QPushButton#pdfBack:hover { border: none; background: transparent; }"
        "QWidget#drivePdfPreview QPushButton#pdfBack:focus { border: none; outline: none; background: transparent; }"
        "QWidget#drivePdfPreview QPushButton:disabled { background: transparent; color: %5; }"
        "QWidget#pdfPageNavigation { background: %7; border: 1px solid %3; border-radius: 8px; }"
        "QLabel#pdfPageCount { color: %5; font-size: 12px; padding-right: 4px; }"
        "QSpinBox#pdfPage { color: %2; background: transparent; border: none; padding: 0; font-size: 12px; selection-background-color: #00856B; }"
        "QSpinBox#pdfPage:focus { background: %4; border-radius: 4px; }"
        "QComboBox#pdfZoom { min-height: 0; padding: 0 28px 0 10px; background: %7; color: %2; border: 1px solid %3; border-radius: 8px; font-size: 12px; }"
        "QComboBox#pdfZoom:hover { border-color: %5; }"
        "QComboBox#pdfZoom:focus { border-color: #00856B; }"
        "QComboBox#pdfZoom::drop-down { border: none; width: 24px; }"
        "QComboBox#pdfZoom::down-arrow { image: url(:/udrive/icons/%8.svg); width: 10px; height: 10px; }"
        "QAbstractScrollArea#pdfCanvas, QWidget#pdfViewport { background: %6; border: none; border-bottom-left-radius: 15px; border-bottom-right-radius: 15px; }"
        "QLabel#pdfPageImage { background: white; border: 1px solid rgba(0, 0, 0, 0.12); }"
        "QLabel#pdfStatus { color: %2; background: %1; border: 1px solid %3; border-radius: 8px; padding: 12px; font-size: 13px; }")
        .arg(surface, text, border, hover, muted, canvas, field, arrow));
    _zoom->view()->setStyleSheet(QStringLiteral(
        "QAbstractItemView { background: %1; color: %2; border: 1px solid %3; padding: 4px; outline: none; }"
        "QAbstractItemView::item { min-height: 24px; padding: 4px 8px; border-radius: 4px; }"
        "QAbstractItemView::item:selected { background: %4; }").arg(surface, text, border, hover));
    _updatingAppearance = false;
}
}
