#include "credentialmanager.h"

#include "account.h"
#include "configfile.h"
#include "theme.h"

#include "common/asserts.h"
#include "common/chronoelapsedtimer.h"

#include <QCborValue>
#include <QLoggingCategory>
#include <QTimer>

#include <chrono>

using namespace std::chrono_literals;

using namespace OCC;

Q_LOGGING_CATEGORY(lcCredentialsManager, "sync.credentials.manager", QtDebugMsg)

namespace {
constexpr auto tiemoutC = 5s;
QString credentialKeyC()
{
    return QString("%1_credentials").arg(Theme::instance()->appName());
}
}

CredentialManager::CredentialManager(Account *acc)
    : QObject(acc)
    , _account(acc)
{
}

CredentialManager::CredentialManager(QObject *parent)
    : QObject(parent)
{
}

QString CredentialManager::scope() const
{
    return _account ? accountKey() : credentialKeyC();
}

QString CredentialManager::scopedKey(const QString &key) const
{
    return scope() + QLatin1Char(':') + key;
}

QString CredentialManager::accountKey() const
{
    if (!_account || _account->url().isEmpty())
        return QString();

    return QString("%1:%2:%3").arg(credentialKeyC(), _account->url().host(), _account->uuid().toString(QUuid::WithoutBraces));
}

CredentialJob *CredentialManager::get(const QString &key)
{
    if (!contains(key)) {
        qCWarning(lcCredentialsManager) << "We don't know" << key << "skipping retrieval from keychain";
        return nullptr;
    }
    const QString scopedKey = this->scopedKey(key);
    qCInfo(lcCredentialsManager) << "get" << scopedKey;
    auto out = new CredentialJob(this, scopedKey);
    out->start();
    return out;
}

QKeychain::Job *CredentialManager::set(const QString &key, const QVariant &data)
{
    OC_ASSERT(!data.isNull());
    const QString scoped = scopedKey(key);
    qCInfo(lcCredentialsManager) << "set" << scoped;
    auto writeJob = new QKeychain::WritePasswordJob(Theme::instance()->appName());
    writeJob->setKey(scoped);
    const bool previouslyKnown = contains(key);
    const auto generation = ++_writeGeneration[key];
    // Register before the queued write starts. Logout must enqueue a delete
    // even when the first write has not yet completed. QKeychain serializes jobs.
    credentialsList().setValue(key, true);

    auto timer = new QTimer(writeJob);
    timer->setInterval(tiemoutC);

    Utility::ChronoElapsedTimer elapsedTimer;
    connect(timer, &QTimer::timeout, writeJob,
        [writeJob, elapsedTimer] { qCWarning(lcCredentialsManager) << "set" << writeJob->key() << "has not yet finished." << elapsedTimer.duration(); });
    connect(writeJob, &QKeychain::WritePasswordJob::finished, this, [writeJob, key, elapsedTimer, this, generation, previouslyKnown] {
        if (writeJob->error() == QKeychain::NoError) {
            qCInfo(lcCredentialsManager) << "added" << writeJob->key() << "after" << elapsedTimer.duration();
        } else {
            qCWarning(lcCredentialsManager) << "Failed to set:" << writeJob->key() << writeJob->errorString() << "after" << elapsedTimer.duration();
            if (_writeGeneration.value(key) == generation && !previouslyKnown)
                credentialsList().remove(key);
        }
    });
    writeJob->setBinaryData(QCborValue::fromVariant(data).toCbor());
    // start is delayed so we can directly call it
    writeJob->start();
    timer->start();

    return writeJob;
}

QKeychain::Job *CredentialManager::remove(const QString &key)
{
    OC_ASSERT(contains(key));
    ++_writeGeneration[key];
    // remove immediately to prevent double invocation by clear()
    credentialsList().remove(key);
    const QString scoped = scopedKey(key);
    qCInfo(lcCredentialsManager) << "del" << scoped;
    auto keychainJob = new QKeychain::DeletePasswordJob(Theme::instance()->appName());
    keychainJob->setKey(scoped);
    connect(keychainJob, &QKeychain::DeletePasswordJob::finished, this, [keychainJob, scoped] {
        if (keychainJob->error() == QKeychain::NoError || keychainJob->error() == QKeychain::EntryNotFound) {
            qCInfo(lcCredentialsManager) << "removed" << scoped;
        } else {
            qCWarning(lcCredentialsManager) << "Failed to remove:" << scoped << keychainJob->errorString();
        }
    });
    // start is delayed so we can directly call it
    keychainJob->start();
    return keychainJob;
}

QVector<QPointer<QKeychain::Job>> CredentialManager::clear(const QString &group)
{
    OC_ENFORCE(_account || !group.isEmpty());
    const auto keys = knownKeys(group);
    QVector<QPointer<QKeychain::Job>> out;
    out.reserve(keys.size());
    for (const auto &key : keys) {
        out << remove(key);
    }
    return out;
}

bool CredentialManager::contains(const QString &key) const
{
    return credentialsList().contains(key);
}
bool CredentialManager::isEmpty()
{
    return knownKeys().isEmpty();
}

QStringList CredentialManager::knownKeys(const QString &group) const
{
    if (group.isEmpty()) {
        return credentialsList().allKeys();
    }
    credentialsList().beginGroup(group);
    const auto keys = credentialsList().allKeys();
    QStringList out;
    out.reserve(keys.size());
    for (const auto &k : keys) {
        out.append(group + QLatin1Char('/') + k);
    }
    credentialsList().endGroup();
    return out;
}

/**
 * Utility function to lazily create the settings (group).
 *
 * IMPORTANT: the underlying storage is a std::unique_ptr, so do *NOT* store this reference anywhere!
 */
QSettings &CredentialManager::credentialsList() const
{
    // delayed init as scope requires a fully inizialised acc
    if (!_credentialsList) {
        _credentialsList = ConfigFile::settingsWithGroup("Credentials/" + scope());
    }
    return *_credentialsList;
}

CredentialJob::CredentialJob(CredentialManager *parent, const QString &scopedKey)
    : QObject(parent)
    , _scopedKey(scopedKey)
{
    connect(this, &CredentialJob::finished, this, &CredentialJob::deleteLater);
}

QString CredentialJob::errorString() const
{
    return _errorString;
}

const QVariant &CredentialJob::data() const
{
    return _data;
}

QKeychain::Error CredentialJob::error() const
{
    return _error;
}

void CredentialJob::start()
{
    _job = new QKeychain::ReadPasswordJob(Theme::instance()->appName());
    _job->setKey(_scopedKey);
    connect(_job, &QKeychain::ReadPasswordJob::finished, this, [this] {
#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
        if (_retryOnKeyChainError && (_job->error() == QKeychain::NoBackendAvailable || _job->error() == QKeychain::OtherError)) {
            // Could be that the backend was not yet available. Wait some extra seconds.
            // (Issues #4274 and #6522)
            // (For kwallet, the error is OtherError instead of NoBackendAvailable, maybe a bug in QtKeychain)
            qCInfo(lcCredentialsManager) << "Backend unavailable (yet?) Retrying in a few seconds." << _job->errorString();
            QTimer::singleShot(10s, this, &CredentialJob::start);
            _retryOnKeyChainError = false;
            return;
        }
#endif
        handleReadResult(_job->error(), _job->errorString(), _job->binaryData());
    });
    _job->start();
}

void CredentialJob::handleReadResult(QKeychain::Error error, const QString &message, const QByteArray &data)
{
    if (error == QKeychain::NoError) {
        QCborParserError parseError;
        const auto obj = QCborValue::fromCbor(data, &parseError);
        if (parseError.error != QCborError::NoError) {
            _error = QKeychain::OtherError;
            _errorString = tr("Failed to parse credentials %1").arg(parseError.errorString());
        } else {
            _data = obj.toVariant();
            if (!_data.isValid()) {
                _error = QKeychain::OtherError;
                _errorString = tr("Stored credentials contain an invalid value.");
            }
        }
    } else {
        qCWarning(lcCredentialsManager) << "Failed to get password" << _scopedKey << message;
        _error = error;
        _errorString = message;
    }
    Q_EMIT finished();
}
