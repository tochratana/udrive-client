// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "udrivepdfdocument.h"
#include <QJsonObject>
#include <QPointer>
#include <QMap>
#include <QPixmap>
#include <QWidget>
#include <memory>

class QLabel;
class QComboBox;
class QSpinBox;
class QAbstractScrollArea;
class QPainter;
class QTemporaryDir;
class QTimer;
class QPushButton;
class QProcess;

namespace OCC {
class PdfTitleLabel;
class UDrivePdfPreview : public QWidget
{
    Q_OBJECT
public:
    explicit UDrivePdfPreview(QWidget *parent = nullptr);
    ~UDrivePdfPreview() override;
    static constexpr int RenderTimeoutMs = 5000;
    void openFile(const QString &path, const QString &name, std::shared_ptr<QTemporaryDir> storage = {});
    void clear();
Q_SIGNALS:
    void backRequested();
    void downloadRequested();
    void pageRendered(int page);
    void previewFailed(const QString &error);
protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void scheduleRender();
    void render();
    void readRenderResult();
    void stopRenderer();
    void failRender(const QString &error);
    void updateControls();
    void updateAppearance();
    void positionStatus();
    void updateViewport();
    void updateScrollRange(int anchorPage, qint64 anchorOffset = 0);
    void navigateToPage(int page);
    void paintPages(QPainter &painter);
    QList<int> neededPages() const;
    QSize pageSize(int page) const;
    qint64 pageTop(int page) const;
    qint64 scrollOffset() const;
    int pageAt(qint64 position) const;
    QMap<int, QSize> _pageSizes;
    QMap<int, QPixmap> _pagePixmaps;
    QMap<int, QString> _pageErrors;
    QList<int> _recentPages;
    QSize _defaultPageSize;
    qreal _scrollScale = 1;
    bool _layoutPending = true;
    int _announcedPage = 0;
    bool _updatingAppearance = false;
    bool _darkAppearance = false;
    QWidget *_toolbar = nullptr;
    QString _path;
    std::shared_ptr<QTemporaryDir> _storage;
    int _pageCount = 0;
    quint64 _generation = 0;
    bool _busy = false;
    bool _failed = false;
    QPointer<QProcess> _process;
    QTimer *_renderTimer;
    QByteArray _output;
    QJsonObject _frame;
    qint64 _expectedBytes = -1;
    quint64 _renderGeneration = 0;
    int _renderPage = 1;
    qreal _renderDpr = 1;
    QTimer *_resizeTimer = nullptr;
    PdfTitleLabel *_title;
    QLabel *_status = nullptr;
    QLabel *_image;
    QLabel *_count;
    QSpinBox *_page;
    QPushButton *_previous;
    QPushButton *_next;
    QComboBox *_zoom;
    QAbstractScrollArea *_scroll = nullptr;
};
}
