// SPDX-License-Identifier: GPL-2.0-or-later
// Used only by the supervised, credential-free PDF worker, never the GUI thread.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include "udrivepdfdocument.h"
#include <QFile>
#include <QObject>
#include <cmath>
#include <limits>

namespace OCC {
using namespace winrt::Windows::Storage::Streams;
struct UDrivePdfDocument::Impl
{
    winrt::Windows::Data::Pdf::PdfDocument document{nullptr};
    InMemoryRandomAccessStream source;
};
UDrivePdfDocument::UDrivePdfDocument(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) { }
UDrivePdfDocument::~UDrivePdfDocument() = default;

std::shared_ptr<UDrivePdfDocument> UDrivePdfDocument::open(const QString &path, QString *error)
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
        if (file.size() > MaximumFileSize) { *error = QObject::tr("This PDF is too large to preview (maximum 100 MiB). Download it to open it on your computer."); return {}; }
        const QByteArray bytes = file.read(MaximumFileSize + 1);
        if (bytes.size() > MaximumFileSize || file.error() != QFileDevice::NoError) { *error = QObject::tr("Could not read this PDF."); return {}; }
        auto impl = std::make_unique<Impl>();
        DataWriter writer(impl->source);
        writer.WriteBytes(winrt::array_view<const uint8_t>(reinterpret_cast<const uint8_t *>(bytes.constData()), reinterpret_cast<const uint8_t *>(bytes.constData()) + bytes.size()));
        writer.StoreAsync().get();
        writer.DetachStream();
        impl->source.Seek(0);
        impl->document = winrt::Windows::Data::Pdf::PdfDocument::LoadFromStreamAsync(impl->source).get();
        if (!impl->document.PageCount() || impl->document.PageCount() > uint32_t(std::numeric_limits<int>::max())) {
            *error = QObject::tr("This PDF has no readable pages."); return {};
        }
        return std::shared_ptr<UDrivePdfDocument>(new UDrivePdfDocument(std::move(impl)));
    } catch (const winrt::hresult_error &) {
        *error = QObject::tr("This PDF is unreadable or password protected. Download it to open it on your computer.");
        return {};
    }
}

int UDrivePdfDocument::pageCount() const { return int(_impl->document.PageCount()); }
QSizeF UDrivePdfDocument::pageSize(int page) const
{
    if (page < 1 || page > pageCount()) return {};
    try {
        const auto pdfPage = _impl->document.GetPage(uint32_t(page - 1));
        const auto size = pdfPage.Size();
        pdfPage.Close();
        if (!std::isfinite(size.Width) || !std::isfinite(size.Height) || size.Width < 0.01 || size.Height < 0.01 || size.Width > 1e7 || size.Height > 1e7) return {};
        return {size.Width, size.Height};
    } catch (const winrt::hresult_error &) { return {}; }
}
QImage UDrivePdfDocument::render(int page, QSize pixels) const
{
    if (pageSize(page).isEmpty() || pixels.isEmpty() || pixels.width() > 4096 || pixels.height() > 4096) return {};
    try {
        const auto pdfPage = _impl->document.GetPage(uint32_t(page - 1));
        InMemoryRandomAccessStream rendered;
        winrt::Windows::Data::Pdf::PdfPageRenderOptions options;
        options.DestinationWidth(uint32_t(pixels.width()));
        options.DestinationHeight(uint32_t(pixels.height()));
        pdfPage.RenderToStreamAsync(rendered, options).get();
        pdfPage.Close();
        const auto length = rendered.Size();
        if (!length || length > 80ULL * 1024 * 1024) return {};
        DataReader reader(rendered.GetInputStreamAt(0));
        if (reader.LoadAsync(uint32_t(length)).get() != length) return {};
        QByteArray bytes(qsizetype(length), Qt::Uninitialized);
        reader.ReadBytes(winrt::array_view<uint8_t>(reinterpret_cast<uint8_t *>(bytes.data()), reinterpret_cast<uint8_t *>(bytes.data()) + bytes.size()));
        return QImage::fromData(bytes, "PNG").convertToFormat(QImage::Format_RGBA8888);
    } catch (const winrt::hresult_error &) { return {}; }
}
}
