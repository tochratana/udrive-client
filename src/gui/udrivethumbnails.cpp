// SPDX-License-Identifier: GPL-2.0-or-later
#include "udrivethumbnails.h"
#include "common/utility.h"
#include "creds/localdevelopment.h"
#include "libsync/account.h"
#include "libsync/creds/abstractcredentials.h"
#include "libsync/networkjobs.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QImageReader>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>
#include <QtConcurrentRun>
#include <utility>

namespace OCC {
namespace {
    constexpr qint64 MaximumBytes = 512 * 1024;

    // Reservations span the entire request/decode lifetime, across all accounts
    // and loader replacement. Only the GUI thread acquires/releases these slots.
    class ThumbnailSlots : public QObject
    {
    public:
        ThumbnailSlots() : QObject(QCoreApplication::instance()) { wakeup.setSingleShot(true); }
        static ThumbnailSlots *instance()
        {
            static QPointer<ThumbnailSlots> coordinator;
            if (!coordinator) coordinator = new ThumbnailSlots;
            return coordinator;
        }
        std::shared_ptr<int> acquire()
        {
            if (_used >= 2) return {};
            ++_used;
            return {new int(0), [guard = QPointer<ThumbnailSlots>(this)](int *slot) {
                delete slot;
                if (guard) { --guard->_used; guard->wakeup.start(0); }
            }};
        }
        QTimer wakeup;

    private:
        int _used = 0;
    };

    QImage decodeThumbnail(QByteArray bytes, int pixels)
    {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(true);
        const auto format = reader.format().toLower();
        // Raster formats only; never feed an SVG/document to a thumbnail renderer.
        if (format != "png" && format != "jpeg" && format != "jpg" && format != "gif" && format != "webp" && format != "bmp")
            return {};
        const QSize size = reader.size();
        if (size.isEmpty() || size.width() > 2048 || size.height() > 2048)
            return {};
        reader.setScaledSize(size.scaled(pixels, pixels, Qt::KeepAspectRatio));
        const auto image = reader.read();
        return image.isNull() ? QImage() : image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
}

UDriveThumbnails::UDriveThumbnails(AccountState *state, QObject *parent, Decoder decoder)
    : QObject(parent)
    , _state(state)
    , _decoder(decoder ? std::move(decoder) : Decoder(decodeThumbnail))
{
    connect(&ThumbnailSlots::instance()->wakeup, &QTimer::timeout, this, &UDriveThumbnails::pump);
    if (state) {
        connect(state, &AccountState::stateChanged, this, [this] {
            if (!available())
                cancel(true);
        });
        connect(state, &QObject::destroyed, this, [this] { cancel(true); });
    }
}

UDriveThumbnails::~UDriveThumbnails()
{
    cancel(true);
}

bool UDriveThumbnails::supports(const QString &name)
{
    const QString extension = name.section(QLatin1Char('.'), -1).toLower();
    return extension == QStringLiteral("jpg") || extension == QStringLiteral("jpeg") || extension == QStringLiteral("png") || extension == QStringLiteral("gif")
        || extension == QStringLiteral("webp") || extension == QStringLiteral("bmp") || extension == QStringLiteral("tif")
        || extension == QStringLiteral("tiff");
}

bool UDriveThumbnails::available() const
{
    return _state && _state->account() && !_state->isSignedOut() && _state->account()->credentials() && _state->account()->credentials()->ready();
}

void UDriveThumbnails::request(const QPersistentModelIndex &index, const QUrl &root, const QString &path, const QString &etag)
{
    if (!available() || !index.isValid() || _queue.size() >= 64 || !root.query().isEmpty() || !root.fragment().isEmpty()
        || !LocalDevelopment::allowsAuthenticatedUrl(root, _state->account()->url()))
        return;
    if (path.isEmpty())
        return;
    for (const auto &part : path.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") || part.contains(QLatin1Char('\\')) || part.contains(QChar::Null))
            return;
    }
    QUrl url = Utility::concatUrlPath(root, path);
    const QString key = url.toString(QUrl::FullyEncoded) + QChar::Null + etag + QChar::Null + QString::number(_pixels);
    if (const auto *pixmap = _cache.object(key)) {
        Q_EMIT ready(index, *pixmap);
        return;
    }
    if (_pending.contains(key) || _failed.contains(key))
        return;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("preview"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("x"), QString::number(_pixels));
    query.addQueryItem(QStringLiteral("y"), QString::number(_pixels));
    query.addQueryItem(QStringLiteral("a"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("processor"), QStringLiteral("fit"));
    if (!etag.isEmpty())
        query.addQueryItem(QStringLiteral("c"), etag);
    url.setQuery(query);
    _pending.insert(key);
    _queue.append({index, url, etag, key, _pixels});
    pump();
}

void UDriveThumbnails::setPixels(int pixels)
{
    if ((pixels != 64 && pixels != 256) || pixels == _pixels) return;
    cancel();
    _pixels = pixels;
}

void UDriveThumbnails::cancel(bool clearCache)
{
    ++_generation;
    _queue.clear();
    _pending.clear();
    _failed.clear();
    _active = 0; // Aborted network jobs; running decoders keep their slots.
    const auto jobs = std::exchange(_jobs, {});
    for (const auto &job : jobs)
        if (job)
            job->abort();
    if (clearCache)
        _cache.clear();
}

void UDriveThumbnails::finish(const Request &request, quint64 generation, const QImage &image)
{
    if (generation != _generation) {
        pump();
        return;
    }
    _pending.remove(request.key);
    if (available() && request.index.isValid() && !image.isNull()) {
        auto pixmap = QPixmap::fromImage(image);
        if (!request.etag.isEmpty())
            _cache.insert(request.key, new QPixmap(pixmap), pixmap.width() * pixmap.height() * 4);
        Q_EMIT ready(request.index, pixmap);
    } else
        _failed.insert(request.key);
    pump();
}

void UDriveThumbnails::pump()
{
    while (available() && _active + _decoding < 2 && !_queue.isEmpty()) {
        auto slot = ThumbnailSlots::instance()->acquire();
        if (!slot) break;
        const auto request = _queue.takeFirst();
        if (!request.index.isValid()) {
            _pending.remove(request.key);
            continue;
        }
        ++_active;
        const quint64 generation = _generation;
        QUrl root = request.url;
        root.setQuery(QString());
        QNetworkRequest headers;
        headers.setRawHeader("Accept", "image/png,image/jpeg,image/gif,image/webp,image/bmp");
        auto *job = new SimpleNetworkJob(_state->account(), root, {}, "GET", QUrlQuery(request.url).queryItems(QUrl::FullyDecoded), headers, this);
        job->setTimeout(std::chrono::seconds(15));
        _jobs.append(job);
        auto bytes = std::make_shared<QByteArray>();
        auto rejected = std::make_shared<bool>(false);
        job->addNewReplyHook([this, job, bytes, rejected, generation](QNetworkReply *reply) {
            bytes->clear();
            *rejected = false;
            reply->setReadBufferSize(64 * 1024);
            connect(reply, &QNetworkReply::metaDataChanged, this, [this, job, reply, rejected, generation] {
                if (generation != _generation || job->reply() != reply)
                    return;
                if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > MaximumBytes) {
                    *rejected = true;
                    job->abort();
                }
            });
            connect(reply, &QNetworkReply::readyRead, this, [this, job, reply, bytes, rejected, generation] {
                if (generation != _generation || job->reply() != reply || *rejected)
                    return;
                bytes->append(reply->read(MaximumBytes + 1 - bytes->size()));
                if (bytes->size() > MaximumBytes) {
                    *rejected = true;
                    job->abort();
                }
            });
        });
        connect(job, &AbstractNetworkJob::finishedSignal, this, [this, job, bytes, rejected, request, generation, slot] {
            if (generation != _generation)
                return;
            _jobs.removeAll(job);
            --_active;
            if (*rejected || job->reply()->error() != QNetworkReply::NoError || job->httpStatusCode() != 200) {
                finish(request, generation, {});
                return;
            }
            bytes->append(job->reply()->read(MaximumBytes + 1 - bytes->size()));
            if (bytes->size() > MaximumBytes) {
                finish(request, generation, {});
                return;
            }
            // The watcher owns the reservation until the decoder really exits.
            // Destroying an account/browser must not release a running slot.
            auto *watcher = new QFutureWatcher<QImage>(ThumbnailSlots::instance());
            ++_decoding;
            connect(watcher, &QFutureWatcher<QImage>::finished, watcher, [loader = QPointer<UDriveThumbnails>(this), watcher, request, generation, slot]() mutable {
                const auto image = watcher->result();
                watcher->deleteLater();
                slot.reset();
                if (loader) {
                    --loader->_decoding;
                    loader->finish(request, generation, image);
                }
            });
            watcher->setFuture(QtConcurrent::run(_decoder, std::move(*bytes), request.pixels));
        });
        job->start();
    }
}
}
