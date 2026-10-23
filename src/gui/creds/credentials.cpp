/*
 * Copyright (C) by Lisa Reese <lisa.reese@kiteworks.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */
#include "credentials.h"
#include "localdevelopment.h"

#include "accessmanager.h"
#include "account.h"
#include "config/appconfig.h"
#include "creds/credentialmanager.h"
#include "oauth.h"
#include "requestauthenticationcontroller.h"
#include "requestauthenticationwidget.h"

#include <QAuthenticator>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkInformation>
#include <QNetworkReply>

#include <chrono>

using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcCredentials, "gui.credentials", QtInfoMsg)

namespace {
constexpr int TokenRefreshMaxRetries = 3;
constexpr std::chrono::seconds TokenRefreshDefaultTimeout = 30s;
const char authenticationFailedC[] = "owncloud-authentication-failed";

// when creating new keys we should probably stick with this idea related to a creds "type" in case we need to add or remove
// other cred types in future. all of the updates to transition to oauth only would have been so much easier if the last round had actually implmented
// separate http and oauth creds instead of rolling all of it into the httpCreds "type".
QString refreshTokenKeyC()
{
    return QString("http/oauthtoken");
}
}

namespace OCC {

namespace {
    class RejectedRequestReply final : public QNetworkReply
    {
    public:
        RejectedRequestReply(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QObject *parent)
            : QNetworkReply(parent)
        {
            setOperation(operation);
            setRequest(request);
            setUrl(request.url());
            open(QIODevice::ReadOnly);
            setError(QNetworkReply::ProtocolInvalidOperationError,
                QObject::tr("Authenticated requests require HTTPS. HTTP is only available in local development mode."));
            QTimer::singleShot(0, this, [this] {
                setFinished(true);
                Q_EMIT errorOccurred(error());
                Q_EMIT finished();
            });
        }
        void abort() override { }

    protected:
        qint64 readData(char *, qint64) override { return -1; }
    };
}

class CredentialsAccessManager : public AccessManager
{
    Q_OBJECT
public:
    // this looks like trouble with the default nullptr on parent
    CredentialsAccessManager(const Credentials *cred, QObject *parent = nullptr)
        : AccessManager(parent)
        , _cred(cred)
    {
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        QNetworkRequest req(request);
        // Includes explicitly supplied Basic/Bearer credentials and cookies:
        // DontAddCredentialsAttribute only suppresses automatic insertion.
        if (!_cred || !_cred->_account || !LocalDevelopment::allowsAuthenticatedUrl(req.url(), _cred->_account->url())) {
            return new RejectedRequestReply(op, req, this);
        }
        // Redirects are validated by the account machinery, never internally
        // forwarded by QNAM to another transport without rechecking policy.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        const bool isAccountHost = _cred && _cred->_account
            && req.url().host().compare(_cred->_account->url().host(), Qt::CaseInsensitive) == 0;
        if (!req.attribute(DontAddCredentialsAttribute).toBool()) {
            if (isAccountHost) {
                // A retried request may still carry the old injected header.
                // Always replace it, including when logout cleared our token.
                req.setRawHeader("Authorization", QByteArray());
                if (_cred && !_cred->_accessToken.isEmpty()) {
                    req.setRawHeader("Authorization", "Bearer " + _cred->_accessToken.toUtf8());
                }
            } else {
                // Do not send account bearer tokens to external download hosts (e.g. S3, CDNs)
                if (req.rawHeader("Authorization").startsWith("Bearer ")) {
                    req.setRawHeader("Authorization", QByteArray());
                }
            }
        }
        return AccessManager::createRequest(op, req, outgoingData);
    }

private:
    // The credentials object dies along with the account, while the QNAM might
    // outlive both.
    // Lisa todo: that statement is very concerning. investigate -> In fact the nam is parented by the creds by default so no, it
    // should not outlive the creds unless there are shenanigans wrt reparenting it.
    QPointer<const Credentials> _cred;
};

Credentials::Credentials(const QString &token, const QString &refreshToken, Account *account)
    : AbstractCredentials(account, account)
    , _accessToken(token)
    , _refreshToken(refreshToken)
    , _ready(false)
    , _openIdConfig(AppConfig().openIdConfig())
{
    if (!token.isEmpty() && !refreshToken.isEmpty())
        _ready = true;
}

Credentials::Credentials(Account *account)
    : Credentials({}, {}, account)
{
}

AccessManager *Credentials::createAccessManager() const
{
    AccessManager *am = new CredentialsAccessManager(this);

    connect(am, &QNetworkAccessManager::authenticationRequired, this, &Credentials::slotAuthentication);

    return am;
}

bool Credentials::ready() const
{
    return _ready;
}


void Credentials::fetchFromKeychain()
{
    _wasEverFetched = true;

    if (!_ready && !_refreshToken.isEmpty()) {
        // This happens if the credentials are still loaded from the keychain, bur we are called
        // here because the auth is invalid, so this means we simply need to refresh the credentials
        refreshAccessToken();
        return;
    }

    if (_ready) {
        Q_EMIT fetched();
    } else {
        fetchCredentialsFromKeychain();
    }
}

void Credentials::handleKeychainError(const QString &message)
{
    qCWarning(lcCredentials) << message;

    _fetchErrorString = message;
    _accessToken.clear();
    _ready = false;
    Q_EMIT fetched();
}

void Credentials::fetchCredentialsFromKeychain()
{
    const auto generation = _authenticationGeneration;
    auto job = createKeychainReadJob();
    if (!job) {
        handleKeychainError("get credentials job is null - most likely the key does not exist in the credentials manager");
        return;
    }
    connect(job, &CredentialJob::finished, this, [guard = QPointer<CredentialJob>(job), this, generation] {
        auto *job = guard.data();
        if (!job || generation != _authenticationGeneration)
            return;
        if (job->error() != QKeychain::NoError) {
            handleKeychainError(job->errorString());
            return;
        }
        const auto data = job->data().toString();
        if (!data.isEmpty()) {
            _refreshToken = data;
            refreshAccessToken();
        } else {
            handleKeychainError("get credentials data is empty");
        }
    });
}

CredentialJob *Credentials::createKeychainReadJob()
{
    return _account ? _account->credentialManager()->get(refreshTokenKeyC()) : nullptr;
}

// Lisa todo: this is called periodically, find the trigger. it's coming mostly from the jobs :(
bool Credentials::stillValid(QNetworkReply *reply)
{
    // The function is called in order to determine whether we need to ask the user for a password
    // since we are using OAuth, we already started a refresh in slotAuthentication, at least in theory, ensure the auth is started.
    // If the refresh fails, we are going to Q_EMIT authenticationFailed ourselves
    if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
        slotAuthentication(reply, nullptr);
    }
    return true;
}

void Credentials::slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator)
{
    qCDebug(lcCredentials) << Q_FUNC_INFO << reply;
    if (!_ready)
        return;
    Q_UNUSED(authenticator)
    // Because of issue #4326, we need to set the login and password manually at every requests
    // Thus, if we reach this signal, those credentials were invalid and we terminate.
    qCWarning(lcCredentials) << "Stop request: Authentication failed for " << reply->url().toString() << reply->request().rawHeader("Original-Request-ID");
    reply->setProperty(authenticationFailedC, true);

    if (!_oAuthJob) {
        qCInfo(lcCredentials) << "Refreshing token";
        refreshAccessToken();
    }
}

bool Credentials::refreshAccessToken()
{
    if (_refreshToken.isEmpty())
        return false;
    if (_oAuthJob)
        return true;

    refreshAccessTokenInternal();
    return true;
}

void Credentials::handleRefreshError(QNetworkReply::NetworkError error, const QString &message)
{
    Q_UNUSED(message);

    if (_oAuthJob)
        _oAuthJob->deleteLater();
    _oAuthJob.clear();
    _tokenRefreshRetriesCount++;
    std::chrono::seconds timeout = {};

    if (!networkAvailable()) {
        _tokenRefreshRetriesCount = 0;
        timeout = TokenRefreshDefaultTimeout;
    } else {
        switch (error) {
        case QNetworkReply::ContentNotFoundError:
            // 404: bigip f5?
            timeout = 0s;
            break;
        case QNetworkReply::HostNotFoundError:
            [[fallthrough]];
        case QNetworkReply::TimeoutError:
            [[fallthrough]];
        // Qt reports OperationCanceledError if the request timed out
        case QNetworkReply::OperationCanceledError:
            [[fallthrough]];
        case QNetworkReply::TemporaryNetworkFailureError:
            [[fallthrough]];
        // VPN not ready?
        case QNetworkReply::ConnectionRefusedError:
            _tokenRefreshRetriesCount = 0;
            [[fallthrough]];
        default:
            timeout = TokenRefreshDefaultTimeout;
        }
    }

    if (_tokenRefreshRetriesCount >= TokenRefreshMaxRetries) {
        qCWarning(lcCredentials) << "Too many failed refreshes" << _tokenRefreshRetriesCount << "-> log out";
        finishFailedRefresh();
        _tokenRefreshRetriesCount = 0;
        return;
    }
    const auto generation = _authenticationGeneration;
    QTimer::singleShot(timeout, this, [this, generation] {
        if (generation == _authenticationGeneration)
            refreshAccessTokenInternal();
    });
    Q_EMIT authenticationFailed();
}

void Credentials::handleRefreshSuccess(const QString &accessToken, const QString &refreshToken)
{
    if (_oAuthJob)
        _oAuthJob->deleteLater();
    _oAuthJob.clear();
    _tokenRefreshRetriesCount = 0;
    if (refreshToken.isEmpty()) {
        qCWarning(lcCredentials) << "Refresh job succeeded but refreshToken is empty -> log out";
        finishFailedRefresh();
        return;
    }
    _refreshToken = refreshToken;
    if (!accessToken.isNull()) {
        _ready = true;
        _accessToken = accessToken;
        persist();
    }
    Q_EMIT fetched();
}

void Credentials::finishFailedRefresh()
{
    forgetSensitiveData();
    Q_EMIT authenticationFailed();
    Q_EMIT fetched();
}

bool Credentials::networkAvailable()
{
    auto qni = QNetworkInformation::instance();
    if (!qni)
        return true; // this can allegedly happen on Linux
    if (qni->reachability() == QNetworkInformation::Reachability::Disconnected || qni->reachability() == QNetworkInformation::Reachability::Unknown)
        return false;
    if (qni->isBehindCaptivePortal())
        return false;
    return true;
}

void Credentials::refreshAccessTokenInternal()
{
    if (!_account || _refreshToken.isEmpty() || _oAuthJob)
        return;

    // parent with nam to ensure we reset when the nam is reset
    // todo: #22 - the parenting here is highly questionable, as is the use of the shared account ptr
    _oAuthJob = new AccountBasedOAuth(_account, _openIdConfig, this);
    const auto generation = _authenticationGeneration;
    const QPointer<AccountBasedOAuth> job = _oAuthJob;
    connect(job, &AccountBasedOAuth::refreshError, this, [this, job, generation](QNetworkReply::NetworkError error, const QString &message) {
        if (generation == _authenticationGeneration && job && _oAuthJob == job)
            handleRefreshError(error, message);
    });
    connect(job, &AccountBasedOAuth::refreshFinished, this, [this, job, generation](const QString &token, const QString &refreshToken) {
        if (generation == _authenticationGeneration && job && _oAuthJob == job)
            handleRefreshSuccess(token, refreshToken);
    });

    Q_EMIT authenticationStarted();
    if (generation == _authenticationGeneration && _oAuthJob == job && job)
        job->refreshAuthentication(_refreshToken);
}

void Credentials::askFromUser()
{
    // I think this can happen when the re-auth process has already quasi started and is waiting for user input, but
    // we get a prompt to log in again. I am not quite sure as it's very hard to reproduce.
    if (_requestAuth || !_account) {
        // let the existing instance ride
        return;
    }

    // the widget is parented to the AccountModalWidget when it's installed in the main window.
    // it will be cleaned up there - this is not a leak
    RequestAuthenticationWidget *widget = new RequestAuthenticationWidget();
    connectAuthenticationController(new RequestAuthenticationController(widget, this));
    _requestAuth->startAuthentication(_account);
}

void Credentials::connectAuthenticationController(RequestAuthenticationController *controller)
{
    _requestAuth = controller;
    // we should not connect to the failed signal as we are forcing the user to make the decision using the gui.
    // if they log in successfully, we get the success signal.
    // if the auth fails the gui stays until the user gets it right or clicks the stay logged out button.
    const auto generation = _authenticationGeneration;
    const QPointer<RequestAuthenticationController> guard = controller;
    connect(controller, &RequestAuthenticationController::authenticationSucceeded, this,
        [this, guard, generation](const QString &token, const QString &refreshToken) {
            if (generation == _authenticationGeneration && guard && _requestAuth == guard)
                askFromUserSucceeded(token, refreshToken);
        });
    connect(controller, &RequestAuthenticationController::requestLogout, this, [this, guard, generation] {
        if (generation == _authenticationGeneration && guard && _requestAuth == guard)
            askFromUserLogout();
    });
}

void Credentials::askFromUserSucceeded(const QString &token, const QString &refreshToken)
{
    if (_requestAuth) {
        _requestAuth->deleteLater();
        _requestAuth.clear();
    }
    _accessToken = token;
    _refreshToken = refreshToken;
    _ready = true;
    persist();
    Q_EMIT fetched();
}

void Credentials::askFromUserLogout()
{
    forgetSensitiveData();
    Q_EMIT requestLogout();
}

void Credentials::invalidateToken()
{
    qCWarning(lcCredentials) << "Invalidating the credentials";

    _accessToken.clear();
    _ready = false;

    // clear the session cookie.
    _account->clearCookieJar();

    if (!_refreshToken.isEmpty()) {
        // Only invalidate the access_token but keep the _refreshToken in the keychain
        // (when coming from forgetSensitiveData, the _refreshToken is cleared)
        return;
    }

    // i hoped we could change this to a new, more useful identifier but since the value is related to what is in the keychain, I don't
    // think this can be changed easily so I'm leaving it for now.
    _account->credentialManager()->clear("http");

    // let QNAM forget about the previous credentials it may have been using
    // This needs to be done later in the event loop because we might be called (directly or
    // indirectly) from QNetworkAccessManagerPrivate::authenticationRequired, which itself
    // is a called from a BlockingQueuedConnection from the Qt HTTP thread. And clearing the
    // cache needs to synchronize again with the HTTP thread.
    QTimer::singleShot(0, _account, &Account::clearAMCache);
}

void Credentials::forgetSensitiveData()
{
    ++_authenticationGeneration;
    _tokenRefreshRetriesCount = 0;
    if (_oAuthJob) {
        disconnect(_oAuthJob, nullptr, this, nullptr);
        _oAuthJob->deleteLater();
        _oAuthJob.clear();
    }
    if (_requestAuth) {
        disconnect(_requestAuth, nullptr, this, nullptr);
        _requestAuth->cancelAuthentication();
        _requestAuth->deleteLater();
        _requestAuth.clear();
    }
    // need to be done before invalidateToken, so it actually deletes the refresh_token from the keychain
    // to do, make that more explicit
    _refreshToken.clear();
    invalidateToken();
}

void Credentials::persist()
{
    // write secrets to the keychain
        if (!_refreshToken.isEmpty()) {
            _account->credentialManager()->set(refreshTokenKeyC(), _refreshToken);
        } 
}

} // namespace OCC


#include "credentials.moc"
