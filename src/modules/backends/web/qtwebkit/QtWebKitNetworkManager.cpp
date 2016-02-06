/**************************************************************************
* Otter Browser: Web browser controlled by the user, not vice-versa.
* Copyright (C) 2013 - 2016 Michal Dutkiewicz aka Emdek <michal@emdek.pl>
* Copyright (C) 2014 Piotr Wójcik <chocimier@tlen.pl>
* Copyright (C) 2015 Jan Bajer aka bajasoft <jbajer@gmail.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
**************************************************************************/

#include "QtWebKitNetworkManager.h"
#include "QtWebKitFtpListingNetworkReply.h"
#include "../../../../core/AddonsManager.h"
#include "../../../../core/ContentBlockingManager.h"
#include "../../../../core/Console.h"
#include "../../../../core/CookieJar.h"
#include "../../../../core/CookieJarProxy.h"
#include "../../../../core/LocalListingNetworkReply.h"
#include "../../../../core/NetworkCache.h"
#include "../../../../core/NetworkManagerFactory.h"
#include "../../../../core/SettingsManager.h"
#include "../../../../core/Utils.h"
#include "../../../../core/WebBackend.h"
#include "../../../../ui/AuthenticationDialog.h"
#include "../../../../ui/ContentsDialog.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkReply>

namespace Otter
{

WebBackend* QtWebKitNetworkManager::m_backend = NULL;

QtWebKitNetworkManager::QtWebKitNetworkManager(bool isPrivate, CookieJarProxy *cookieJarProxy, QtWebKitWebWidget *parent) : QNetworkAccessManager(parent),
	m_widget(parent),
	m_cookieJar(NULL),
	m_cookieJarProxy(cookieJarProxy),
	m_baseReply(NULL),
	m_contentState(WindowsManager::UnknownContentState),
	m_speed(0),
	m_bytesReceivedDifference(0),
	m_bytesReceived(0),
	m_bytesTotal(0),
	m_isSecure(0),
	m_finishedRequests(0),
	m_startedRequests(0),
	m_updateTimer(0),
	m_doNotTrackPolicy(NetworkManagerFactory::SkipTrackPolicy),
	m_canSendReferrer(true)
{
	NetworkManagerFactory::initialize();

	if (!isPrivate)
	{
		m_cookieJar = NetworkManagerFactory::getCookieJar();
		m_cookieJar->setParent(QCoreApplication::instance());

		QNetworkDiskCache *cache = NetworkManagerFactory::getCache();

		setCache(cache);

		cache->setParent(QCoreApplication::instance());
	}
	else
	{
		m_cookieJar = new CookieJar(true, this);
	}

	if (m_cookieJarProxy)
	{
		m_cookieJarProxy->setParent(this);
	}
	else
	{
		m_cookieJarProxy = new CookieJarProxy(m_cookieJar, parent);
	}

	setCookieJar(m_cookieJarProxy);

	connect(this, SIGNAL(finished(QNetworkReply*)), SLOT(requestFinished(QNetworkReply*)));
	connect(this, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)), this, SLOT(handleAuthenticationRequired(QNetworkReply*,QAuthenticator*)));
	connect(this, SIGNAL(proxyAuthenticationRequired(QNetworkProxy,QAuthenticator*)), this, SLOT(handleProxyAuthenticationRequired(QNetworkProxy,QAuthenticator*)));
	connect(this, SIGNAL(sslErrors(QNetworkReply*,QList<QSslError>)), this, SLOT(handleSslErrors(QNetworkReply*,QList<QSslError>)));
}

void QtWebKitNetworkManager::timerEvent(QTimerEvent *event)
{
	Q_UNUSED(event)

	updateStatus();
}

void QtWebKitNetworkManager::handleAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator)
{
	emit messageChanged(tr("Waiting for authentication…"));

	AuthenticationDialog *authenticationDialog = new AuthenticationDialog(reply->url(), authenticator, m_widget);
	authenticationDialog->setButtonsVisible(false);

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-password")), authenticationDialog->windowTitle(), QString(), QString(), (QDialogButtonBox::Ok | QDialogButtonBox::Cancel), authenticationDialog, m_widget);

	connect(&dialog, SIGNAL(accepted()), authenticationDialog, SLOT(accept()));
	connect(m_widget, SIGNAL(aboutToReload()), &dialog, SLOT(close()));
	connect(NetworkManagerFactory::getInstance(), SIGNAL(authenticated(QAuthenticator*,bool)), authenticationDialog, SLOT(authenticated(QAuthenticator*,bool)));

	m_widget->showDialog(&dialog);

	NetworkManagerFactory::notifyAuthenticated(authenticator, dialog.isAccepted());
}

void QtWebKitNetworkManager::handleProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator)
{
	if (NetworkManagerFactory::isUsingSystemProxyAuthentication())
	{
		authenticator->setUser(QString());

		return;
	}

	emit messageChanged(tr("Waiting for authentication…"));

	AuthenticationDialog *authenticationDialog = new AuthenticationDialog(proxy.hostName(), authenticator, m_widget);
	authenticationDialog->setButtonsVisible(false);

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-password")), authenticationDialog->windowTitle(), QString(), QString(), (QDialogButtonBox::Ok | QDialogButtonBox::Cancel), authenticationDialog, m_widget);

	connect(&dialog, SIGNAL(accepted()), authenticationDialog, SLOT(accept()));
	connect(m_widget, SIGNAL(aboutToReload()), &dialog, SLOT(close()));
	connect(NetworkManagerFactory::getInstance(), SIGNAL(authenticated(QAuthenticator*,bool)), authenticationDialog, SLOT(authenticated(QAuthenticator*,bool)));

	m_widget->showDialog(&dialog);

	NetworkManagerFactory::notifyAuthenticated(authenticator, dialog.isAccepted());
}

void QtWebKitNetworkManager::handleSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
	if (errors.isEmpty())
	{
		reply->ignoreSslErrors(errors);

		return;
	}

	QStringList ignoredErrors = m_widget->getOption(QLatin1String("Security/IgnoreSslErrors"), m_widget->getUrl()).toStringList();
	QStringList messages;
	QList<QSslError> errorsToIgnore;

	for (int i = 0; i < errors.count(); ++i)
	{
		if (errors.at(i).error() != QSslError::NoError)
		{
			m_sslInformation.errors.append(qMakePair(reply->url(), errors.at(i)));

			if (ignoredErrors.contains(errors.at(i).certificate().digest().toBase64()))
			{
				errorsToIgnore.append(errors.at(i));
			}
			else
			{
				messages.append(errors.at(i).errorString());
			}
		}
	}

	if (!errorsToIgnore.isEmpty())
	{
		reply->ignoreSslErrors(errorsToIgnore);
	}

	if (messages.isEmpty())
	{
		return;
	}

	ContentsDialog dialog(Utils::getIcon(QLatin1String("dialog-warning")), tr("Warning"), tr("SSL errors occured, do you want to continue?"), messages.join('\n'), (QDialogButtonBox::Yes | QDialogButtonBox::No), NULL, m_widget);

	if (!m_widget->getUrl().isEmpty())
	{
		dialog.setCheckBox(tr("Do not show this message again"), false);
	}

	connect(m_widget, SIGNAL(aboutToReload()), &dialog, SLOT(close()));

	m_widget->showDialog(&dialog);

	if (dialog.isAccepted())
	{
		reply->ignoreSslErrors(errors);

		if (!m_widget->getUrl().isEmpty() && dialog.getCheckBoxState())
		{
			for (int i = 0; i < errors.count(); ++i)
			{
				const QString digest = errors.at(i).certificate().digest().toBase64();

				if (!ignoredErrors.contains(digest))
				{
					ignoredErrors.append(digest);
				}
			}

			SettingsManager::setValue(QLatin1String("Security/IgnoreSslErrors"), ignoredErrors, m_widget->getUrl());
		}
	}
}

void QtWebKitNetworkManager::resetStatistics()
{
	killTimer(m_updateTimer);
	updateStatus();

	m_dateDownloaded = QDateTime();
	m_sslInformation = WebWidget::SslInformation();
	m_updateTimer = 0;
	m_replies.clear();
	m_blockedRequests.clear();
	m_baseReply = NULL;
	m_speed = 0;
	m_contentState = WindowsManager::UnknownContentState;
	m_bytesReceivedDifference = 0;
	m_bytesReceived = 0;
	m_bytesTotal = 0;
	m_finishedRequests = 0;
	m_startedRequests = 0;
	m_isSecure = 0;

	emit contentStateChanged(m_contentState);
}

void QtWebKitNetworkManager::registerTransfer(QNetworkReply *reply)
{
	if (reply && !reply->isFinished())
	{
		m_transfers.append(reply);

		setParent(NULL);

		connect(reply, SIGNAL(finished()), this, SLOT(transferFinished()));
	}
}

void QtWebKitNetworkManager::downloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

	if (reply && reply == m_baseReply)
	{
		if (m_baseReply->hasRawHeader(QStringLiteral("Location").toLatin1()))
		{
			m_baseReply = NULL;
		}
		else
		{
			if (bytesTotal > 0)
			{
				emit documentLoadProgressChanged(((bytesReceived * 1.0) / bytesTotal) * 100);
			}
			else
			{
				emit documentLoadProgressChanged(-1);
			}
		}
	}

	if (!reply || !m_replies.contains(reply))
	{
		return;
	}

	const QUrl url(reply->url());

	if (url.isValid() && url.scheme() != QLatin1String("data"))
	{
		emit messageChanged(tr("Receiving data from %1…").arg(reply->url().host().isEmpty() ? QLatin1String("localhost") : reply->url().host()));
	}

	const qint64 difference = (bytesReceived - m_replies[reply].first);

	m_replies[reply].first = bytesReceived;

	if (!m_replies[reply].second && bytesTotal > 0)
	{
		m_replies[reply].second = true;

		m_bytesTotal += bytesTotal;
	}

	if (difference <= 0)
	{
		return;
	}

	m_bytesReceived += difference;
	m_bytesReceivedDifference += difference;
}

void QtWebKitNetworkManager::requestFinished(QNetworkReply *reply)
{
	if (reply)
	{
		if (reply == m_baseReply)
		{
			if (reply->sslConfiguration().isNull())
			{
				m_sslInformation.certificates = QList<QSslCertificate>();
				m_sslInformation.cipher = QSslCipher();
			}
			else
			{
				m_sslInformation.certificates = reply->sslConfiguration().peerCertificateChain();
				m_sslInformation.cipher = reply->sslConfiguration().sessionCipher();
			}
		}

		m_replies.remove(reply);
	}

	if (m_replies.isEmpty())
	{
		killTimer(m_updateTimer);

		m_dateDownloaded = QDateTime::currentDateTime();
		m_updateTimer = 0;

		updateStatus();

		if ((m_isSecure == 1 || (m_isSecure == 0 && m_contentState.testFlag(WindowsManager::SecureContentState))) && m_sslInformation.errors.isEmpty())
		{
			m_contentState = WindowsManager::SecureContentState;
		}

		emit contentStateChanged(m_contentState);
	}

	++m_finishedRequests;

	if (reply)
	{
		const QUrl url(reply->url());

		if (url.isValid() && url.scheme() != QLatin1String("data"))
		{
			emit messageChanged(tr("Completed request to %1").arg(url.host().isEmpty() ? QLatin1String("localhost") : reply->url().host()));
		}

		disconnect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgress(qint64,qint64)));
	}
}

void QtWebKitNetworkManager::transferFinished()
{
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

	if (reply)
	{
		m_transfers.removeAll(reply);

		reply->deleteLater();

		if (m_transfers.isEmpty())
		{
			if (m_widget)
			{
				setParent(m_widget);
			}
			else
			{
				deleteLater();
			}
		}
	}
}

void QtWebKitNetworkManager::updateStatus()
{
	m_speed = (m_bytesReceivedDifference * 2);
	m_bytesReceivedDifference = 0;

	emit statusChanged(m_finishedRequests, m_startedRequests, m_bytesReceived, m_bytesTotal, m_speed);
}

void QtWebKitNetworkManager::updateOptions(const QUrl &url)
{
	if (!m_backend)
	{
		m_backend = AddonsManager::getWebBackend(QLatin1String("qtwebkit"));
	}

	QString acceptLanguage = SettingsManager::getValue(QLatin1String("Network/AcceptLanguage"), url).toString();
	acceptLanguage = ((acceptLanguage.isEmpty()) ? QLatin1String(" ") : acceptLanguage.replace(QLatin1String("system"), QLocale::system().bcp47Name()));

	m_acceptLanguage = ((acceptLanguage == NetworkManagerFactory::getAcceptLanguage()) ? QString() : acceptLanguage);
	m_userAgent = m_backend->getUserAgent(m_widget ? NetworkManagerFactory::getUserAgent(m_widget->getOption(QLatin1String("Network/UserAgent"), url).toString()).value : QString());

	const QString doNotTrackPolicyValue = SettingsManager::getValue(QLatin1String("Network/DoNotTrackPolicy"), url).toString();

	if (doNotTrackPolicyValue == QLatin1String("allow"))
	{
		m_doNotTrackPolicy = NetworkManagerFactory::AllowToTrackPolicy;
	}
	else if (doNotTrackPolicyValue == QLatin1String("doNotAllow"))
	{
		m_doNotTrackPolicy = NetworkManagerFactory::DoNotAllowToTrackPolicy;
	}
	else
	{
		m_doNotTrackPolicy = NetworkManagerFactory::SkipTrackPolicy;
	}

	m_canSendReferrer = SettingsManager::getValue(QLatin1String("Network/EnableReferrer"), url).toBool();

	const QString generalCookiesPolicyValue = SettingsManager::getValue(QLatin1String("Network/CookiesPolicy"), url).toString();
	CookieJar::CookiesPolicy generalCookiesPolicy = CookieJar::AcceptAllCookies;

	if (generalCookiesPolicyValue == QLatin1String("ignore"))
	{
		generalCookiesPolicy = CookieJar::IgnoreCookies;
	}
	else if (generalCookiesPolicyValue == QLatin1String("readOnly"))
	{
		generalCookiesPolicy = CookieJar::ReadOnlyCookies;
	}
	else if (generalCookiesPolicyValue == QLatin1String("acceptExisting"))
	{
		generalCookiesPolicy = CookieJar::AcceptExistingCookies;
	}

	const QString thirdPartyCookiesPolicyValue = SettingsManager::getValue(QLatin1String("Network/ThirdPartyCookiesPolicy"), url).toString();
	CookieJar::CookiesPolicy thirdPartyCookiesPolicy = CookieJar::AcceptAllCookies;

	if (thirdPartyCookiesPolicyValue == QLatin1String("ignore"))
	{
		thirdPartyCookiesPolicy = CookieJar::IgnoreCookies;
	}
	else if (thirdPartyCookiesPolicyValue == QLatin1String("readOnly"))
	{
		thirdPartyCookiesPolicy = CookieJar::ReadOnlyCookies;
	}
	else if (thirdPartyCookiesPolicyValue == QLatin1String("acceptExisting"))
	{
		thirdPartyCookiesPolicy = CookieJar::AcceptExistingCookies;
	}

	const QString keepModeValue = SettingsManager::getValue(QLatin1String("Network/CookiesKeepMode"), url).toString();
	CookieJar::KeepMode keepMode = CookieJar::KeepUntilExpiresMode;

	if (keepModeValue == QLatin1String("keepUntilExit"))
	{
		keepMode = CookieJar::KeepUntilExitMode;
	}
	else if (keepModeValue == QLatin1String("ask"))
	{
		keepMode = CookieJar::AskIfKeepMode;
	}

	m_cookieJarProxy->setup(SettingsManager::getValue(QLatin1String("Network/ThirdPartyCookiesAcceptedHosts"), url).toStringList(), SettingsManager::getValue(QLatin1String("Network/ThirdPartyCookiesRejectedHosts"), url).toStringList(), generalCookiesPolicy, thirdPartyCookiesPolicy, keepMode);
}

void QtWebKitNetworkManager::setFormRequest(const QUrl &url)
{
	m_formRequestUrl = url;
}

void QtWebKitNetworkManager::setWidget(QtWebKitWebWidget *widget)
{
	setParent(widget);

	m_widget = widget;

	m_cookieJarProxy->setWidget(widget);
}

QtWebKitNetworkManager* QtWebKitNetworkManager::clone()
{
	return new QtWebKitNetworkManager((cache() == NULL), m_cookieJarProxy->clone(NULL), NULL);
}

QNetworkReply* QtWebKitNetworkManager::createRequest(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, QIODevice *outgoingData)
{
	if (request.url() == m_formRequestUrl)
	{
		m_formRequestUrl = QUrl();

		m_widget->openFormRequest(request.url(), operation, outgoingData);

		return QNetworkAccessManager::createRequest(QNetworkAccessManager::GetOperation, QNetworkRequest());
	}

	const QString host = request.url().host();

	if (!m_widget->isNavigating())
	{
		const QVector<int> profiles(m_widget->getContentBlockingProfiles());

		if (!profiles.isEmpty())
		{
			const QByteArray acceptHeader = request.rawHeader(QByteArray("Accept"));
			const QString path(request.url().path());
			ContentBlockingManager::ResourceType resourceType(ContentBlockingManager::OtherType);

			if (!m_baseReply)
			{
				resourceType = ContentBlockingManager::MainFrameType;
			}
			else if (m_widget->hasFrame(request.url()))
			{
				resourceType = ContentBlockingManager::SubFrameType;
			}
			else if (acceptHeader.contains(QByteArray("image/")) || path.endsWith(QLatin1String(".png")) || path.endsWith(QLatin1String(".jpg")) || path.endsWith(QLatin1String(".gif")))
			{
				resourceType = ContentBlockingManager::ImageType;
			}
			else if (acceptHeader.contains(QByteArray("script/")) || path.endsWith(QLatin1String(".js")))
			{
				resourceType = ContentBlockingManager::ScriptType;
			}
			else if (acceptHeader.contains(QByteArray("text/css")) || path.endsWith(QLatin1String(".css")))
			{
				resourceType = ContentBlockingManager::StyleSheetType;
			}
			else if (acceptHeader.contains(QByteArray("object")))
			{
				resourceType = ContentBlockingManager::ObjectType;
			}
			else if (request.rawHeader(QByteArray("X-Requested-With")) == QByteArray("XMLHttpRequest"))
			{
				resourceType = ContentBlockingManager::XmlHttpRequestType;
			}

			if (ContentBlockingManager::isUrlBlocked(profiles, m_widget->getUrl(), request.url(), resourceType))
			{
				Console::addMessage(QCoreApplication::translate("main", "Blocked request"), Otter::NetworkMessageCategory, LogMessageLevel, request.url().toString());

				QUrl url = QUrl();
				url.setScheme(QLatin1String("http"));

				if (m_blockedRequests.contains(host))
				{
					++m_blockedRequests[host];
				}
				else
				{
					m_blockedRequests[host] = 1;
				}

				return QNetworkAccessManager::createRequest(QNetworkAccessManager::GetOperation, QNetworkRequest(url));
			}
		}
	}

	++m_startedRequests;

	QNetworkRequest mutableRequest(request);

	if (!m_canSendReferrer)
	{
		mutableRequest.setRawHeader(QStringLiteral("Referer").toLatin1(), QByteArray());
	}

	if (operation == PostOperation && mutableRequest.header(QNetworkRequest::ContentTypeHeader).isNull())
	{
		mutableRequest.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("application/x-www-form-urlencoded"));
	}

	if (NetworkManagerFactory::isWorkingOffline())
	{
		mutableRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysCache);
	}
	else if (m_doNotTrackPolicy != NetworkManagerFactory::SkipTrackPolicy)
	{
		mutableRequest.setRawHeader(QByteArray("DNT"), QByteArray((m_doNotTrackPolicy == NetworkManagerFactory::DoNotAllowToTrackPolicy) ? "1" : "0"));
	}

	mutableRequest.setRawHeader(QStringLiteral("Accept-Language").toLatin1(), (m_acceptLanguage.isEmpty() ? NetworkManagerFactory::getAcceptLanguage().toLatin1() : m_acceptLanguage.toLatin1()));
	mutableRequest.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);

	emit messageChanged(tr("Sending request to %1…").arg(host));

	QNetworkReply *reply = NULL;

	if (operation == GetOperation && request.url().isLocalFile() && QFileInfo(request.url().toLocalFile()).isDir())
	{
		reply = new LocalListingNetworkReply(this, request);
	}
	else if (operation == GetOperation && request.url().scheme() == QLatin1String("ftp"))
	{
		reply = new QtWebKitFtpListingNetworkReply(request, this);
	}
	else
	{
		reply = QNetworkAccessManager::createRequest(operation, mutableRequest, outgoingData);
	}

	if (!m_baseReply)
	{
		m_baseReply = reply;
	}

	if (m_isSecure >= 0)
	{
		const QString scheme = reply->url().scheme();

		if (scheme == QLatin1String("https"))
		{
			m_isSecure = 1;
		}
		else if (scheme == QLatin1String("http"))
		{
			m_isSecure = -1;
		}
	}

	m_replies[reply] = qMakePair(0, false);

	connect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgress(qint64,qint64)));

	if (m_updateTimer == 0)
	{
		m_updateTimer = startTimer(500);
	}

	return reply;
}

CookieJar* QtWebKitNetworkManager::getCookieJar()
{
	return m_cookieJar;
}

WebWidget::SslInformation QtWebKitNetworkManager::getSslInformation() const
{
	return m_sslInformation;
}

QHash<QByteArray, QByteArray> QtWebKitNetworkManager::getHeaders() const
{
	QHash<QByteArray, QByteArray> headers;

	if (m_baseReply)
	{
		const QList<QNetworkReply::RawHeaderPair> rawHeaders = m_baseReply->rawHeaderPairs();

		for (int i = 0; i < rawHeaders.count(); ++i)
		{
			headers[rawHeaders.at(i).first] = rawHeaders.at(i).second;
		}
	}

	return headers;
}

QVariantHash QtWebKitNetworkManager::getStatistics() const
{
	QVariantHash statistics;
	statistics[QLatin1String("dateDownloaded")] = m_dateDownloaded;
	statistics[QLatin1String("bytesReceived")] = m_bytesReceived;
	statistics[QLatin1String("bytesTotal")] = m_bytesTotal;
	statistics[QLatin1String("requestsBlocked")] = m_blockedRequests.count();
	statistics[QLatin1String("requestsFinished")] = m_finishedRequests;
	statistics[QLatin1String("requestsStarted")] = m_startedRequests;
	statistics[QLatin1String("speed")] = m_speed;

	return statistics;
}

WindowsManager::ContentStates QtWebKitNetworkManager::getContentState() const
{
	return m_contentState;
}

bool QtWebKitNetworkManager::isLoading() const
{
	return !m_replies.isEmpty();
}

}
