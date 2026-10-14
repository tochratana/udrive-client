// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "udrivepdfdocument.h"
#include <QJsonObject>
#include <QPointer>
#include <QWidget>
#include <memory>

class QLabel;
class QComboBox;
class QSpinBox;
class QScrollArea;
class QTemporaryDir;
class QTimer;
class QPushButton;
class QProcess;

namespace OCC {
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
    void pageRendered(int page);
    void previewFailed(const QString &error);
protected:
    void resizeEvent(QResizeEvent *event) override;
private:
    void scheduleRender();
    void render();
    void readRenderResult();
    void stopRenderer();
    void failRender(const QString &error);
    void updateControls();
    QString _path;
    std::shared_ptr<QTemporaryDir> _storage;
    int _pageCount = 0;
    quint64 _generation = 0;
    bool _busy = false;
    bool _pending = false;
    bool _failed = false;
    QPointer<QProcess> _process;
    QTimer *_renderTimer;
    QByteArray _output;
    QJsonObject _frame;
    qint64 _expectedBytes = -1;
    quint64 _renderGeneration = 0;
    int _renderPage = 1;
    qreal _renderDpr = 1;
    QTimer *_resizeTimer;
    QLabel *_title;
    QLabel *_status;
    QLabel *_image;
    QLabel *_count;
    QSpinBox *_page;
    QPushButton *_previous;
    QPushButton *_next;
    QComboBox *_zoom;
    QScrollArea *_scroll;
};
}
