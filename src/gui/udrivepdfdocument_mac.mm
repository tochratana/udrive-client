// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivepdfdocument.h"
#include <CoreGraphics/CoreGraphics.h>
#include <QFile>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <limits>

namespace OCC {
struct UDrivePdfDocument::Impl
{
    CGPDFDocumentRef document = nullptr;
    int pages = 0;
    ~Impl() { if (document) CGPDFDocumentRelease(document); }
};

UDrivePdfDocument::UDrivePdfDocument(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) { }
UDrivePdfDocument::~UDrivePdfDocument() = default;

std::shared_ptr<UDrivePdfDocument> UDrivePdfDocument::open(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    if (file.size() > MaximumFileSize) {
        *error = QObject::tr("This PDF is too large to preview (maximum 100 MiB). Download it to open it on your computer.");
        return {};
    }
    // An immutable snapshot also prevents a sync update from changing pages mid-preview.
    const auto bytes = file.read(MaximumFileSize + 1);
    if (bytes.size() > MaximumFileSize || file.error() != QFileDevice::NoError) {
        *error = QObject::tr("Could not read this PDF."); return {};
    }
    auto data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()), bytes.size());
    if (!data) { *error = QObject::tr("Not enough memory to preview this PDF."); return {}; }
    auto provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    auto impl = std::make_unique<Impl>();
    if (provider) {
        impl->document = CGPDFDocumentCreateWithProvider(provider);
        CGDataProviderRelease(provider);
    }
    if (!impl->document) { *error = QObject::tr("This file is not a readable PDF."); return {}; }
    if (!CGPDFDocumentIsUnlocked(impl->document)) {
        *error = QObject::tr("This PDF is password protected. Download it to open it on your computer."); return {};
    }
    const auto count = CGPDFDocumentGetNumberOfPages(impl->document);
    if (!count || count > static_cast<size_t>(std::numeric_limits<int>::max())) {
        *error = QObject::tr("This PDF has no readable pages."); return {};
    }
    impl->pages = static_cast<int>(count);
    return std::shared_ptr<UDrivePdfDocument>(new UDrivePdfDocument(std::move(impl)));
}

int UDrivePdfDocument::pageCount() const { return _impl->pages; }

QSizeF UDrivePdfDocument::pageSize(int page) const
{
    if (page < 1 || page > pageCount()) return {};
    const auto pdfPage = CGPDFDocumentGetPage(_impl->document, page);
    if (!pdfPage) return {};
    const auto box = CGPDFPageGetBoxRect(pdfPage, kCGPDFCropBox);
    QSizeF size(box.size.width, box.size.height);
    const int rotation = CGPDFPageGetRotationAngle(pdfPage);
    if (rotation % 180) size.transpose();
    if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() < 0.01 || size.height() < 0.01
        || size.width() > 1e7 || size.height() > 1e7) return {};
    return size;
}

QImage UDrivePdfDocument::render(int page, QSize pixels) const
{
    const QSizeF logicalSize = pageSize(page);
    if (logicalSize.isEmpty() || pixels.isEmpty() || pixels.width() > 4096 || pixels.height() > 4096) return {};
    QImage image(pixels, QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    image.fill(Qt::white);
    auto colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) return {};
    auto context = CGBitmapContextCreate(image.bits(), image.width(), image.height(), 8, image.bytesPerLine(), colorSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colorSpace);
    if (!context) return {};
    const auto pdfPage = CGPDFDocumentGetPage(_impl->document, page);
    if (!pdfPage) { CGContextRelease(context); return {}; }
    // CGPDFPageGetDrawingTransform only scales pages down. Apply the output
    // scale explicitly so zooming in and Retina rendering enlarge the content
    // along with the page, while the native transform handles crop/rotation.
    const qreal scale = std::min(image.width() / logicalSize.width(), image.height() / logicalSize.height());
    CGContextTranslateCTM(context, (image.width() - logicalSize.width() * scale) / 2,
        (image.height() - logicalSize.height() * scale) / 2);
    CGContextScaleCTM(context, scale, scale);
    const auto transform = CGPDFPageGetDrawingTransform(pdfPage, kCGPDFCropBox,
        CGRectMake(0, 0, logicalSize.width(), logicalSize.height()), 0, true);
    CGContextConcatCTM(context, transform);
    CGContextDrawPDFPage(context, pdfPage);
    CGContextRelease(context);
    return image;
}
}
