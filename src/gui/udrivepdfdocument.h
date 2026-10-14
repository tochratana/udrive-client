// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>
#include <memory>

namespace OCC {
// Read-only rendering: PDF links, scripts and embedded actions are never executed.
class UDrivePdfDocument
{
public:
    static constexpr qint64 MaximumFileSize = 100 * 1024 * 1024;
    static std::shared_ptr<UDrivePdfDocument> open(const QString &path, QString *error);
    ~UDrivePdfDocument();
    int pageCount() const;
    QSizeF pageSize(int page) const;
    QImage render(int page, QSize pixels) const;
private:
    struct Impl;
    explicit UDrivePdfDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};
}
