// Copyright 2007-2021 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include <QSslConfiguration>
#include <QtCore/QtGlobal>

#ifdef Q_OS_WIN
#	include "win.h"
#endif

#include "ServerHandler.h"

#include "AudioInput.h"
#include "AudioOutput.h"
#include "Cert.h"
#include "Connection.h"
#include "Database.h"
#include "HostAddress.h"
#include "MainWindow.h"
#include "Message.h"
#include "Net.h"
#include "NetworkConfig.h"
#include "OSInfo.h"
#include "PacketDataStream.h"
#include "ProtoUtils.h"
#include "RichTextEditor.h"
#include "SSL.h"
#include "ServerResolver.h"
#include "ServerResolverRecord.h"
#include "User.h"
#include "Utils.h"
#include "Global.h"

#include <QPainter>
#include <QtCore/QtEndian>
#include <QtGui/QImageReader>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QUdpSocket>

#include <openssl/crypto.h>

#ifdef Q_OS_WIN
// <delayimp.h> is not protected with an include guard on MinGW, resulting in
// redefinitions if the PCH header is used.
// The workaround consists in including the header only if _DELAY_IMP_VER
// (defined in the header) is not defined.
#	ifndef _DELAY_IMP_VER
#		include <delayimp.h>
#	endif
#	include <qos2.h>
#	include <wincrypt.h>
#	include <winsock2.h>
#else
#	if defined(Q_OS_FREEBSD) || defined(Q_OS_OPENBSD)
#		include <netinet/in.h>
#	endif
#	include <netinet/ip.h>
#	include <sys/socket.h>
#endif

// Init ServerHandler::nextConnectionID
int ServerHandler::nextConnectionID = -1;
QMutex ServerHandler::nextConnectionIDMutex;

ServerHandlerMessageEvent::ServerHandlerMessageEvent(const QByteArray &msg, unsigned int mtype, bool flush)
	: QEvent(static_cast< QEvent::Type >(SERVERSEND_EVENT)) {
	qbaMsg = msg;
	uiType = mtype;
	bFlush = flush;
}

#ifdef Q_OS_WIN
static HANDLE loadQoS() {
	HANDLE hQoS = nullptr;

	HRESULT hr = E_FAIL;

// We don't support delay-loading QoS on MinGW. Only enable it for MSVC.
#	ifdef _MSC_VER
	__try {
		hr = __HrLoadAllImportsForDll("qwave.dll");
	}

	__except (EXCEPTION_EXECUTE_HANDLER) {
		hr = E_FAIL;
	}
#	endif

	if (!SUCCEEDED(hr)) {
		qWarning("ServerHandler: Failed to load qWave.dll, no QoS available");
	} else {
		QOS_VERSION qvVer;
		qvVer.MajorVersion = 1;
		qvVer.MinorVersion = 0;

		if (!QOSCreateHandle(&qvVer, &hQoS)) {
			qWarning("ServerHandler: Failed to create QOS2 handle");
			hQoS = nullptr;
		} else {
			qWarning("ServerHandler: QOS2 loaded");
		}
	}
	return hQoS;
}
#endif

ServerHandler::ServerHandler() : database(new Database(QLatin1String("ServerHandler"))) {
	cConnection.reset();
	qusUdp                  = nullptr;
	bStrong                 = false;
	usPort                  = 0;
	bUdp                    = true;
	tConnectionTimeoutTimer = nullptr;
	uiVersion               = Version::UNKNOWN;
	iInFlightTCPPings       = 0;

	// assign connection ID
	{
		QMutexLocker lock(&nextConnectionIDMutex);
		nextConnectionID++;
		connectionID = nextConnectionID;
	}

	// Historically, the qWarning line below initialized OpenSSL for us.
	// It used to have this comment:
	//
	//     "For some strange reason, on Win32, we have to call
	//      supportsSsl before the cipher list is ready."
	//
	// Now, OpenSSL is initialized in main() via MumbleSSL::initialize(),
	// but since it's handy to have the OpenSSL version available, we
	// keep this one around as well.
	qWarning("OpenSSL Support: %d (%s)", QSslSocket::supportsSsl(), SSLeay_version(SSLEAY_VERSION));

	MumbleSSL::addSystemCA();

	{
		QList< QSslCipher > ciphers = MumbleSSL::ciphersFromOpenSSLCipherString(Global::get().s.qsSslCiphers);
		if (ciphers.isEmpty()) {
			qFatal("Invalid 'net/sslciphers' config option. Either the cipher string is invalid or none of the ciphers "
				   "are available:: \"%s\"",
				   qPrintable(Global::get().s.qsSslCiphers));
		}

		QSslConfiguration config = QSslConfiguration::defaultConfiguration();
		config.setCiphers(ciphers);
		QSslConfiguration::setDefaultConfiguration(config);

		QStringList pref;
		foreach (QSslCipher c, ciphers) { pref << c.name(); }
		qWarning("ServerHandler: TLS cipher preference is \"%s\"", qPrintable(pref.join(QLatin1String(":"))));
	}

#ifdef Q_OS_WIN
	hQoS = loadQoS();
	if (hQoS)
		Connection::setQoS(hQoS);
#endif

	connect(this, SIGNAL(pingRequested()), this, SLOT(sendPingInternal()), Qt::QueuedConnection);
}

ServerHandler::~ServerHandler() {
	wait();
	cConnection.reset();
#ifdef Q_OS_WIN
	if (hQoS) {
		QOSCloseHandle(hQoS);
		Connection::setQoS(nullptr);
	}
#endif
}

void ServerHandler::customEvent(QEvent *evt) {
	if (evt->type() != SERVERSEND_EVENT)
		return;

	ServerHandlerMessageEvent *shme = static_cast< ServerHandlerMessageEvent * >(evt);

	ConnectionPtr connection(cConnection);
	if (connection) {
		if (shme->qbaMsg.size() > 0) {
			connection->sendMessage(shme->qbaMsg);
			if (shme->bFlush)
				connection->forceFlush();
		} else {
			exit(0);
		}
	}
}

int ServerHandler::getConnectionID() const {
	return connectionID;
}

void ServerHandler::udpReady() {
	const unsigned int UDP_MAX_SIZE = 2048;
	while (qusUdp->hasPendingDatagrams()) {
		char encrypted[UDP_MAX_SIZE];
		char buffer[UDP_MAX_SIZE];
		unsigned int buflen = static_cast< unsigned int >(qusUdp->pendingDatagramSize());

		if (buflen > UDP_MAX_SIZE) {
			// Discard datagrams that exceed our buffer's size as we'd have to trim them down anyways and it is not very
			// likely that the data is valid in the trimmed down form.
			// As we're using a maxSize of 0 it is okay to pass nullptr as the data buffer. Qt's docs (5.15) ensures
			// that a maxSize of 0 means discarding the datagram.
			qusUdp->readDatagram(nullptr, 0);
			continue;
		}

		QHostAddress senderAddr;
		quint16 senderPort;
		qusUdp->readDatagram(encrypted, buflen, &senderAddr, &senderPort);

		if (!(HostAddress(senderAddr) == HostAddress(qhaRemote)) || (senderPort != usResolvedPort))
			continue;

		ConnectionPtr connection(cConnection);
		if (!connection)
			continue;

		if (!connection->csCrypt->isValid())
			continue;

		if (buflen < 5)
			continue;

		if (!connection->csCrypt->decrypt(reinterpret_cast< const unsigned char * >(encrypted),
										  reinterpret_cast< unsigned char * >(buffer), buflen)) {
			if (connection->csCrypt->tLastGood.elapsed() > 5000000ULL) {
				if (connection->csCrypt->tLastRequest.elapsed() > 5000000ULL) {
					connection->csCrypt->tLastRequest.restart();
					MumbleProto::CryptSetup mpcs;
					sendMessage(mpcs);
				}
			}
			continue;
		}

		PacketDataStream pds(buffer + 1, buflen - 5);

		MessageHandler::UDPMessageType msgType = static_cast< MessageHandler::UDPMessageType >((buffer[0] >> 5) & 0x7);
		unsigned int msgFlags                  = buffer[0] & 0x1f;

		switch (msgType) {
			case MessageHandler::UDPPing: {
				quint64 t;
				pds >> t;
				accUDP(static_cast< double >(tTimestamp.elapsed() - t) / 1000.0);
			} break;
			case MessageHandler::UDPVoiceCELTAlpha:
			case MessageHandler::UDPVoiceCELTBeta:
			case MessageHandler::UDPVoiceSpeex:
			case MessageHandler::UDPVoiceOpus:
				handleVoicePacket(msgFlags, pds, msgType);
				break;
			default:
				break;
		}
	}
}

void ServerHandler::handleVoicePacket(unsigned int msgFlags, PacketDataStream &pds,
									  MessageHandler::UDPMessageType type) {
	unsigned int uiSession;
	pds >> uiSession;
	ClientUser *p     = ClientUser::get(uiSession);
	AudioOutputPtr ao = Global::get().ao;
	if (ao && p && !(((msgFlags & 0x1f) == 2) && Global::get().s.bWhisperFriends && p->qsFriendName.isEmpty())) {
		unsigned int iSeq;
		pds >> iSeq;
		QByteArray qba;
		qba.reserve(pds.left() + 1);
		qba.append(static_cast< char >(msgFlags));
		qba.append(pds.dataBlock(pds.left()));
		ao->addFrameToBuffer(p, qba, iSeq, type);
	}
}

void ServerHandler::sendMessage(const char *data, int len, bool force) {
	STACKVAR(unsigned char, crypto, len + 4);

	QMutexLocker qml(&qmUdp);

	if (!qusUdp)
		return;

	ConnectionPtr connection(cConnection);
	if (!connection || !connection->csCrypt->isValid())
		return;

	if (!force && (NetworkConfig::TcpModeEnabled() || !bUdp)) {
		QByteArray qba;

		qba.resize(len + 6);
		unsigned char *uc                      = reinterpret_cast< unsigned char * >(qba.data());
		*reinterpret_cast< quint16 * >(&uc[0]) = qToBigEndian(static_cast< quint16 >(MessageHandler::UDPTunnel));
		*reinterpret_cast< quint32 * >(&uc[2]) = qToBigEndian(static_cast< quint32 >(len));
		memcpy(uc + 6, data, len);

		QApplication::postEvent(this, new ServerHandlerMessageEvent(qba, MessageHandler::UDPTunnel, true));
	} else {
		if (!connection->csCrypt->encrypt(reinterpret_cast< const unsigned char * >(data), crypto, len)) {
			return;
		}
		qusUdp->writeDatagram(reinterpret_cast< const char * >(crypto), len + 4, qhaRemote, usResolvedPort);
	}
}

void ServerHandler::sendProtoMessage(const ::google::protobuf::Message &msg, unsigned int msgType) {
	QByteArray qba;

	if (QThread::currentThread() != thread()) {
		Connection::messageToNetwork(msg, msgType, qba);
		ServerHandlerMessageEvent *shme = new ServerHandlerMessageEvent(qba, 0, false);
		QApplication::postEvent(this, shme);
	} else {
		ConnectionPtr connection(cConnection);
		if (!connection)
			return;

		connection->sendMessage(msg, msgType, qba);
	}
}

bool ServerHandler::isConnected() const {
	// If the digest isn't empty, then we are currently connected to a server (the digest being a hash
	// of the server's certificate)
	return !qbaDigest.isEmpty();
}

bool ServerHandler::hasSynchronized() const {
	return serverSynchronized;
}

void ServerHandler::setServerSynchronized(bool synchronized) {
	serverSynchronized = synchronized;
}

void ServerHandler::hostnameResolved() {
	ServerResolver *sr                    = qobject_cast< ServerResolver * >(QObject::sender());
	QList< ServerResolverRecord > records = sr->records();

	// Exit the ServerHandler thread's event loop with an
	// error code in case our hostname lookup failed.
	if (records.isEmpty()) {
		exit(-1);
		return;
	}

	// Create the list of target host:port pairs
	// that the ServerHandler should try to connect to.
	QList< ServerAddress > ql;
	QHash< ServerAddress, QString > qh;
	foreach (ServerResolverRecord record, records) {
		foreach (HostAddress addr, record.addresses()) {
			auto sa = ServerAddress(addr, record.port());
			ql.append(sa);
			qh[sa] = record.hostname();
		}
	}
	qlAddresses = ql;
	qhHostnames = qh;

	// Exit the event loop with 'success' status code,
	// to continue connecting to the server.
	exit(0);
}

void ServerHandler::run() {
	// Resolve the hostname...
	{
		ServerResolver sr;
		QObject::connect(&sr, SIGNAL(resolved()), this, SLOT(hostnameResolved()));
		sr.resolve(qsHostName, usPort);
		int ret = exec();
		if (ret < 0) {
			qWarning("ServerHandler: failed to resolve hostname");
			emit error(QAbstractSocket::HostNotFoundError, tr("Unable to resolve hostname"));
			return;
		}
	}

	QList< ServerAddress > targetAddresses(qlAddresses);
	bool shouldTryNextTargetServer = true;
	do {
		saTargetServer = qlAddresses.takeFirst();

		tConnectionTimeoutTimer = nullptr;
		qbaDigest               = QByteArray();
		bStrong                 = true;
		qtsSock                 = new QSslSocket(this);
		qtsSock->setPeerVerifyName(qhHostnames[saTargetServer]);

		if (!Global::get().s.bSuppressIdentity && CertWizard::validateCert(Global::get().s.kpCertificate)) {
			qtsSock->setPrivateKey(Global::get().s.kpCertificate.second);
			qtsSock->setLocalCertificate(Global::get().s.kpCertificate.first.at(0));
			QSslConfiguration config       = qtsSock->sslConfiguration();
			QList< QSslCertificate > certs = config.caCertificates();
			certs << Global::get().s.kpCertificate.first;
			config.setCaCertificates(certs);
			qtsSock->setSslConfiguration(config);
		}

		{
			ConnectionPtr connection(new Connection(this, qtsSock));
			cConnection = connection;

			// Technically it isn't necessary to reset this flag here since a ServerHandler will not be used
			// for multiple connections in a row but just in case that at some point it will, we'll reset the
			// flag here.
			serverSynchronized = false;

			qlErrors.clear();
			qscCert.clear();

			connect(qtsSock, SIGNAL(encrypted()), this, SLOT(serverConnectionConnected()));
			connect(qtsSock, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this,
					SLOT(serverConnectionStateChanged(QAbstractSocket::SocketState)));
			connect(connection.get(), SIGNAL(connectionClosed(QAbstractSocket::SocketError, const QString &)), this,
					SLOT(serverConnectionClosed(QAbstractSocket::SocketError, const QString &)));
			connect(connection.get(), SIGNAL(message(unsigned int, const QByteArray &)), this,
					SLOT(message(unsigned int, const QByteArray &)));
			connect(connection.get(), SIGNAL(handleSslErrors(const QList< QSslError > &)), this,
					SLOT(setSslErrors(const QList< QSslError > &)));
		}
		bUdp = false;


#if QT_VERSION >= 0x050500
		qtsSock->setProtocol(QSsl::TlsV1_0OrLater);
#elif QT_VERSION >= 0x050400
		// In Qt 5.4, QSsl::SecureProtocols is equivalent
		// to "TLSv1.0 or later", which we require.
		qtsSock->setProtocol(QSsl::SecureProtocols);
#else
		qtsSock->setProtocol(QSsl::TlsV1_0);
#endif

		qtsSock->connectToHost(saTargetServer.host.toAddress(), saTargetServer.port);

		tTimestamp.restart();

		// Setup ping timer;
		QTimer *ticker = new QTimer(this);
		connect(ticker, SIGNAL(timeout()), this, SLOT(sendPing()));
		ticker->start(Global::get().s.iPingIntervalMsec);

		Global::get().mw->rtLast = MumbleProto::Reject_RejectType_None;

		accUDP = accTCP = accClean;

		uiVersion   = Version::UNKNOWN;
		qsRelease   = QString();
		qsOS        = QString();
		qsOSVersion = QString();

		int ret = exec();
		if (ret == -2) {
			shouldTryNextTargetServer = true;
		} else {
			shouldTryNextTargetServer = false;
		}

		if (qusUdp) {
			QMutexLocker qml(&qmUdp);

#ifdef Q_OS_WIN
			if (hQoS) {
				if (!QOSRemoveSocketFromFlow(hQoS, 0, dwFlowUDP, 0)) {
					qWarning("ServerHandler: Failed to remove UDP from QoS. QOSRemoveSocketFromFlow() failed with "
							 "error %lu!",
							 GetLastError());
				}

				dwFlowUDP = 0;
			}
#endif
			delete qusUdp;
			qusUdp = nullptr;
		}

		ticker->stop();

		ConnectionPtr cptr(cConnection);
		if (cptr) {
			cptr->disconnectSocket(true);
		}

		cConnection.reset();
		while (!cptr.unique()) {
			msleep(100);
		}
		delete qtsSock;
		delete tConnectionTimeoutTimer;
	} while (shouldTryNextTargetServer && !qlAddresses.isEmpty());
}

#ifdef Q_OS_WIN
extern DWORD WinVerifySslCert(const QByteArray &cert);
#endif

void ServerHandler::setSslErrors(const QList< QSslError > &errors) {
	ConnectionPtr connection(cConnection);
	if (!connection)
		return;

	qscCert                      = connection->peerCertificateChain();
	QList< QSslError > newErrors = errors;

#ifdef Q_OS_WIN
	bool bRevalidate = false;
	QList< QSslError > errorsToRemove;
	foreach (const QSslError &e, errors) {
		switch (e.error()) {
			case QSslError::UnableToGetLocalIssuerCertificate:
			case QSslError::SelfSignedCertificateInChain:
				bRevalidate = true;
				errorsToRemove << e;
				break;
			default:
				break;
		}
	}

	if (bRevalidate) {
		QByteArray der    = qscCert.first().toDer();
		DWORD errorStatus = WinVerifySslCert(der);
		if (errorStatus == CERT_TRUST_NO_ERROR) {
			foreach (const QSslError &e, errorsToRemove) { newErrors.removeOne(e); }
		}
		if (newErrors.isEmpty()) {
			connection->proceedAnyway();
			return;
		}
	}
#endif

	bStrong = false;
	if ((qscCert.size() > 0)
		&& (QString::fromLatin1(qscCert.at(0).digest(QCryptographicHash::Sha1).toHex())
			== database->getDigest(qsHostName, usPort)))
		connection->proceedAnyway();
	else
		qlErrors = newErrors;
}

void ServerHandler::sendPing() {
	emit pingRequested();
}

void ServerHandler::sendPingInternal() {
	ConnectionPtr connection(cConnection);
	if (!connection)
		return;

	if (qtsSock->state() != QAbstractSocket::ConnectedState) {
		return;
	}

	// Ensure the TLS handshake has completed before sending pings.
	if (!qtsSock->isEncrypted()) {
		return;
	}

	if (Global::get().s.iMaxInFlightTCPPings > 0 && iInFlightTCPPings >= Global::get().s.iMaxInFlightTCPPings) {
		serverConnectionClosed(QAbstractSocket::UnknownSocketError, tr("Server is not responding to TCP pings"));
		return;
	}

	quint64 t = tTimestamp.elapsed();

	if (qusUdp) {
		unsigned char buffer[256];
		PacketDataStream pds(buffer + 1, 255);
		buffer[0] = MessageHandler::UDPPing << 5;
		pds << t;
		sendMessage(reinterpret_cast< const char * >(buffer), pds.size() + 1, true);
	}

	MumbleProto::Ping mpp;

	mpp.set_timestamp(t);
	mpp.set_good(connection->csCrypt->uiGood);
	mpp.set_late(connection->csCrypt->uiLate);
	mpp.set_lost(connection->csCrypt->uiLost);
	mpp.set_resync(connection->csCrypt->uiResync);


	if (boost::accumulators::count(accUDP)) {
		mpp.set_udp_ping_avg(static_cast< float >(boost::accumulators::mean(accUDP)));
		mpp.set_udp_ping_var(static_cast< float >(boost::accumulators::variance(accUDP)));
	}
	mpp.set_udp_packets(static_cast< int >(boost::accumulators::count(accUDP)));

	if (boost::accumulators::count(accTCP)) {
		mpp.set_tcp_ping_avg(static_cast< float >(boost::accumulators::mean(accTCP)));
		mpp.set_tcp_ping_var(static_cast< float >(boost::accumulators::variance(accTCP)));
	}
	mpp.set_tcp_packets(static_cast< int >(boost::accumulators::count(accTCP)));

	sendMessage(mpp);

	iInFlightTCPPings += 1;
}

void ServerHandler::message(unsigned int msgType, const QByteArray &qbaMsg) {
	const char *ptr = qbaMsg.constData();
	if (msgType == MessageHandler::UDPTunnel) {
		if (qbaMsg.length() < 1)
			return;

		MessageHandler::UDPMessageType umsgType = static_cast< MessageHandler::UDPMessageType >((ptr[0] >> 5) & 0x7);
		unsigned int msgFlags                   = ptr[0] & 0x1f;
		PacketDataStream pds(qbaMsg.constData() + 1, qbaMsg.size());

		switch (umsgType) {
			case MessageHandler::UDPVoiceCELTAlpha:
			case MessageHandler::UDPVoiceCELTBeta:
			case MessageHandler::UDPVoiceSpeex:
			case MessageHandler::UDPVoiceOpus:
				handleVoicePacket(msgFlags, pds, umsgType);
				break;
			default:
				break;
		}
	} else if (msgType == MessageHandler::Ping) {
		MumbleProto::Ping msg;
		if (msg.ParseFromArray(qbaMsg.constData(), qbaMsg.size())) {
			ConnectionPtr connection(cConnection);
			if (!connection)
				return;

			// Reset in-flight TCP ping counter to 0.
			// We've received a ping. That means the
			// connection is still OK.
			iInFlightTCPPings = 0;

			connection->csCrypt->uiRemoteGood   = msg.good();
			connection->csCrypt->uiRemoteLate   = msg.late();
			connection->csCrypt->uiRemoteLost   = msg.lost();
			connection->csCrypt->uiRemoteResync = msg.resync();
			accTCP(static_cast< double >(tTimestamp.elapsed() - msg.timestamp()) / 1000.0);

			if (((connection->csCrypt->uiRemoteGood == 0) || (connection->csCrypt->uiGood == 0)) && bUdp
				&& (tTimestamp.elapsed() > 20000000ULL)) {
				bUdp = false;
				if (!NetworkConfig::TcpModeEnabled()) {
					if ((connection->csCrypt->uiRemoteGood == 0) && (connection->csCrypt->uiGood == 0))
						Global::get().mw->msgBox(
							tr("UDP packets cannot be sent to or received from the server. Switching to TCP mode."));
					else if (connection->csCrypt->uiRemoteGood == 0)
						Global::get().mw->msgBox(
							tr("UDP packets cannot be sent to the server. Switching to TCP mode."));
					else
						Global::get().mw->msgBox(
							tr("UDP packets cannot be received from the server. Switching to TCP mode."));

					database->setUdp(qbaDigest, false);
				}
			} else if (!bUdp && (connection->csCrypt->uiRemoteGood > 3) && (connection->csCrypt->uiGood > 3)) {
				bUdp = true;
				if (!NetworkConfig::TcpModeEnabled()) {
					Global::get().mw->msgBox(
						tr("UDP packets can be sent to and received from the server. Switching back to UDP mode."));

					database->setUdp(qbaDigest, true);
				}
			}
		}
	} else {
		ServerHandlerMessageEvent *shme = new ServerHandlerMessageEvent(qbaMsg, msgType, false);
		QApplication::postEvent(Global::get().mw, shme);
	}
}

void ServerHandler::disconnect() {
	// Actual TCP object is in a different thread, so signal it
	QByteArray qbaBuffer;
	ServerHandlerMessageEvent *shme = new ServerHandlerMessageEvent(qbaBuffer, 0, false);
	QApplication::postEvent(this, shme);
}

void ServerHandler::serverConnectionClosed(QAbstractSocket::SocketError err, const QString &reason) {
	Connection *c = cConnection.get();
	if (!c)
		return;
	if (c->bDisconnectedEmitted)
		return;
	c->bDisconnectedEmitted = true;

	AudioOutputPtr ao = Global::get().ao;
	if (ao)
		ao->wipe();

	// Try next server in the list if possible.
	// Otherwise, emit disconnect and exit with
	// a normal status code.
	if (!qlAddresses.isEmpty()) {
		if (err == QAbstractSocket::ConnectionRefusedError || err == QAbstractSocket::SocketTimeoutError) {
			qWarning("ServerHandler: connection attempt to %s:%i failed: %s (%li); trying next server....",
					 qPrintable(saTargetServer.host.toString()), static_cast< int >(saTargetServer.port),
					 qPrintable(reason), static_cast< long >(err));
			exit(-2);
			return;
		}
	}

	// Having 2 signals here that basically fire at the same time is wanted behavior!
	// See the documentation of "aboutToDisconnect" for an explanation.
	emit aboutToDisconnect(err, reason);
	emit disconnected(err, reason);

	exit(0);
}

void ServerHandler::serverConnectionTimeoutOnConnect() {
	ConnectionPtr connection(cConnection);
	if (connection)
		connection->disconnectSocket(true);

	serverConnectionClosed(QAbstractSocket::SocketTimeoutError, tr("Connection timed out"));
}

void ServerHandler::serverConnectionStateChanged(QAbstractSocket::SocketState state) {
	if (state == QAbstractSocket::ConnectingState) {
		// Start timer for connection timeout during connect after resolving is completed
		tConnectionTimeoutTimer = new QTimer();
		connect(tConnectionTimeoutTimer, SIGNAL(timeout()), this, SLOT(serverConnectionTimeoutOnConnect()));
		tConnectionTimeoutTimer->setSingleShot(true);
		tConnectionTimeoutTimer->start(Global::get().s.iConnectionTimeoutDurationMsec);
	} else if (state == QAbstractSocket::ConnectedState) {
		// Start TLS handshake
		qtsSock->startClientEncryption();
	}
}

void ServerHandler::serverConnectionConnected() {
	ConnectionPtr connection(cConnection);
	if (!connection)
		return;

#if QT_VERSION >= QT_VERSION_CHECK(5, 7, 0)
	// The ephemeralServerKey property is only a non-null key, if forward secrecy is used.
	// See also https://doc.qt.io/qt-5/qsslconfiguration.html#ephemeralServerKey
	connectionUsesPerfectForwardSecrecy = !qtsSock->sslConfiguration().ephemeralServerKey().isNull();
#endif

	iInFlightTCPPings = 0;

	tConnectionTimeoutTimer->stop();

	if (Global::get().s.bQoS)
		connection->setToS();

	qscCert   = connection->peerCertificateChain();
	qscCipher = connection->sessionCipher();

	if (!qscCert.isEmpty()) {
		// Get the server's immediate SSL certificate
		const QSslCertificate &qsc = qscCert.first();
		qbaDigest                  = sha1(qsc.publicKey().toDer());
		bUdp                       = database->getUdp(qbaDigest);
	} else {
		// Shouldn't reach this
		qCritical("Server must have a certificate. Dropping connection");
		disconnect();
		return;
	}

	MumbleProto::Version mpv;
	mpv.set_release(u8(Version::getRelease()));
	MumbleProto::setVersion(mpv, Version::get());

	if (!Global::get().s.bHideOS) {
		mpv.set_os(u8(OSInfo::getOS()));
		mpv.set_os_version(u8(OSInfo::getOSDisplayableVersion()));
	}

	sendMessage(mpv);

	MumbleProto::Authenticate mpa;
	mpa.set_username(u8(qsUserName));
	mpa.set_password(u8(qsPassword));

	QStringList tokens = database->getTokens(qbaDigest);
	foreach (const QString &qs, tokens)
		mpa.add_tokens(u8(qs));

	QMap< int, CELTCodec * >::const_iterator i;
	for (i = Global::get().qmCodecs.constBegin(); i != Global::get().qmCodecs.constEnd(); ++i)
		mpa.add_celt_versions(i.key());
#ifdef USE_OPUS
	mpa.set_opus(true);
#else
	mpa.set_opus(false);
#endif
	sendMessage(mpa);

	{
		QMutexLocker qml(&qmUdp);

		qhaRemote      = connection->peerAddress();
		qhaLocal       = connection->localAddress();
		usResolvedPort = connection->peerPort();
		if (qhaLocal.isNull()) {
			qFatal("ServerHandler: qhaLocal is unexpectedly a null addr");
		}

		qusUdp = new QUdpSocket(this);
		if (!qusUdp) {
			qFatal("ServerHandler: qusUdp is unexpectedly a null addr");
		}
		if (Global::get().s.bUdpForceTcpAddr) {
			qusUdp->bind(qhaLocal, 0);
		} else {
			if (qhaRemote.protocol() == QAbstractSocket::IPv6Protocol) {
				qusUdp->bind(QHostAddress(QHostAddress::AnyIPv6), 0);
			} else {
				qusUdp->bind(QHostAddress(QHostAddress::Any), 0);
			}
		}

		connect(qusUdp, SIGNAL(readyRead()), this, SLOT(udpReady()));

		if (Global::get().s.bQoS) {
#if defined(Q_OS_UNIX)
			int val = 0xe0;
			if (setsockopt(static_cast< int >(qusUdp->socketDescriptor()), IPPROTO_IP, IP_TOS, &val, sizeof(val))) {
				val = 0x80;
				if (setsockopt(static_cast< int >(qusUdp->socketDescriptor()), IPPROTO_IP, IP_TOS, &val, sizeof(val)))
					qWarning("ServerHandler: Failed to set TOS for UDP Socket");
			}
#	if defined(SO_PRIORITY)
			socklen_t optlen = sizeof(val);
			if (getsockopt(static_cast< int >(qusUdp->socketDescriptor()), SOL_SOCKET, SO_PRIORITY, &val, &optlen)
				== 0) {
				if (val == 0) {
					val = 6;
					setsockopt(static_cast< int >(qusUdp->socketDescriptor()), SOL_SOCKET, SO_PRIORITY, &val,
							   sizeof(val));
				}
			}
#	endif
#elif defined(Q_OS_WIN)
			if (hQoS) {
				struct sockaddr_in addr;
				memset(&addr, 0, sizeof(addr));
				addr.sin_family = AF_INET;
				addr.sin_port = htons(usPort);
				addr.sin_addr.s_addr = htonl(qhaRemote.toIPv4Address());

				dwFlowUDP = 0;
				if (!QOSAddSocketToFlow(hQoS, qusUdp->socketDescriptor(), reinterpret_cast< sockaddr * >(&addr),
										QOSTrafficTypeVoice, QOS_NON_ADAPTIVE_FLOW,
										reinterpret_cast< PQOS_FLOWID >(&dwFlowUDP)))
					qWarning("ServerHandler: Failed to add UDP to QOS");
			}
#endif
		}
	}

	emit connected();
}

void ServerHandler::setConnectionInfo(const QString &host, unsigned short port, const QString &username,
									  const QString &pw) {
	qsHostName = host;
	usPort     = port;
	qsUserName = username;
	qsPassword = pw;
}

void ServerHandler::getConnectionInfo(QString &host, unsigned short &port, QString &username, QString &pw) const {
	host     = qsHostName;
	port     = usPort;
	username = qsUserName;
	pw       = qsPassword;
}

bool ServerHandler::isStrong() const {
	return bStrong;
}

void ServerHandler::requestUserStats(unsigned int uiSession, bool statsOnly) {
	MumbleProto::UserStats mpus;
	mpus.set_session(uiSession);
	mpus.set_stats_only(statsOnly);
	sendMessage(mpus);
}

void ServerHandler::joinChannel(unsigned int uiSession, unsigned int channel) {
	static const QStringList EMPTY;

	joinChannel(uiSession, channel, EMPTY);
}

void ServerHandler::joinChannel(unsigned int uiSession, unsigned int channel,
								const QStringList &temporaryAccessTokens) {
	MumbleProto::UserState mpus;
	mpus.set_session(uiSession);
	mpus.set_channel_id(channel);

	foreach (const QString &tmpToken, temporaryAccessTokens) {
		mpus.add_temporary_access_tokens(tmpToken.toUtf8().constData());
	}

	sendMessage(mpus);
}

void ServerHandler::startListeningToChannel(int channel) {
	startListeningToChannels({ channel });
}

void ServerHandler::startListeningToChannels(const QList< int > &channelIDs) {
	if (channelIDs.isEmpty()) {
		return;
	}

	MumbleProto::UserState mpus;
	mpus.set_session(Global::get().uiSession);

	foreach (int currentChannel, channelIDs) {
		// The naming of the function is a bit unfortunate but what this does is to add
		// the channel ID to the message field listening_channel_add
		mpus.add_listening_channel_add(currentChannel);
	}

	sendMessage(mpus);
}

void ServerHandler::stopListeningToChannel(int channel) {
	stopListeningToChannels({ channel });
}

void ServerHandler::stopListeningToChannels(const QList< int > &channelIDs) {
	if (channelIDs.isEmpty()) {
		return;
	}

	MumbleProto::UserState mpus;
	mpus.set_session(Global::get().uiSession);

	foreach (int currentChannel, channelIDs) {
		// The naming of the function is a bit unfortunate but what this does is to add
		// the channel ID to the message field listening_channel_remove
		mpus.add_listening_channel_remove(currentChannel);
	}

	sendMessage(mpus);
}

void ServerHandler::createChannel(unsigned int parent_id, const QString &name, const QString &description,
								  unsigned int position, bool temporary, unsigned int maxUsers) {
	MumbleProto::ChannelState mpcs;
	mpcs.set_parent(parent_id);
	mpcs.set_name(u8(name));
	mpcs.set_description(u8(description));
	mpcs.set_position(position);
	mpcs.set_temporary(temporary);
	mpcs.set_max_users(maxUsers);
	sendMessage(mpcs);
}

void ServerHandler::requestBanList() {
	MumbleProto::BanList mpbl;
	mpbl.set_query(true);
	sendMessage(mpbl);
}

void ServerHandler::requestUserList() {
	MumbleProto::UserList mpul;
	sendMessage(mpul);
}

void ServerHandler::requestACL(unsigned int channel) {
	MumbleProto::ACL mpacl;
	mpacl.set_channel_id(channel);
	mpacl.set_query(true);
	sendMessage(mpacl);
}

void ServerHandler::registerUser(unsigned int uiSession) {
	MumbleProto::UserState mpus;
	mpus.set_session(uiSession);
	mpus.set_user_id(0);
	sendMessage(mpus);
}

void ServerHandler::kickBanUser(unsigned int uiSession, const QString &reason, bool ban) {
	MumbleProto::UserRemove mpur;
	mpur.set_session(uiSession);
	mpur.set_reason(u8(reason));
	mpur.set_ban(ban);
	sendMessage(mpur);
}

void ServerHandler::sendUserTextMessage(unsigned int uiSession, const QString &message_) {
	MumbleProto::TextMessage mptm;
	mptm.add_session(uiSession);
	mptm.set_message(u8(message_));
	sendMessage(mptm);
}

void ServerHandler::sendChannelTextMessage(unsigned int channel, const QString &message_, bool tree) {
	MumbleProto::TextMessage mptm;
	if (tree) {
		mptm.add_tree_id(channel);
	} else {
		mptm.add_channel_id(channel);

		if (message_ == QString::fromUtf8(Global::get().ccHappyEaster + 10))
			Global::get().bHappyEaster = true;
	}
	mptm.set_message(u8(message_));
	sendMessage(mptm);
}

void ServerHandler::setUserComment(unsigned int uiSession, const QString &comment) {
	MumbleProto::UserState mpus;
	mpus.set_session(uiSession);
	mpus.set_comment(u8(comment));
	sendMessage(mpus);
}

void ServerHandler::setUserTexture(unsigned int uiSession, const QByteArray &qba) {
	QByteArray texture;

	if ((uiVersion >= Version::fromComponents(1, 2, 2)) || qba.isEmpty()) {
		texture = qba;
	} else {
		QByteArray raw = qba;

		QBuffer qb(&raw);
		qb.open(QIODevice::ReadOnly);

		QImageReader qir;
		qir.setDecideFormatFromContent(false);

		QByteArray fmt;
		if (!RichTextImage::isValidImage(qba, fmt)) {
			return;
		}

		qir.setFormat(fmt);
		qir.setDevice(&qb);

		QSize sz                 = qir.size();
		const int TEX_MAX_WIDTH  = 600;
		const int TEX_MAX_HEIGHT = 60;
		const int TEX_RGBA_SIZE  = TEX_MAX_WIDTH * TEX_MAX_HEIGHT * 4;
		sz.scale(TEX_MAX_WIDTH, TEX_MAX_HEIGHT, Qt::KeepAspectRatio);
		qir.setScaledSize(sz);

		QImage tex = qir.read();
		if (tex.isNull()) {
			return;
		}

		raw = QByteArray(TEX_RGBA_SIZE, 0);
		QImage img(reinterpret_cast< unsigned char * >(raw.data()), TEX_MAX_WIDTH, TEX_MAX_HEIGHT,
				   QImage::Format_ARGB32);

		QPainter imgp(&img);
		imgp.setRenderHint(QPainter::Antialiasing);
		imgp.setRenderHint(QPainter::TextAntialiasing);
		imgp.setCompositionMode(QPainter::CompositionMode_SourceOver);
		imgp.drawImage(0, 0, tex);

		texture = qCompress(QByteArray(reinterpret_cast< const char * >(img.bits()), TEX_RGBA_SIZE));
	}

	MumbleProto::UserState mpus;
	mpus.set_session(uiSession);
	mpus.set_texture(blob(texture));
	sendMessage(mpus);

	if (!texture.isEmpty()) {
		database->setBlob(sha1(texture), texture);
	}
}

void ServerHandler::setTokens(const QStringList &tokens) {
	MumbleProto::Authenticate msg;
	foreach (const QString &qs, tokens)
		msg.add_tokens(u8(qs));
	sendMessage(msg);
}

void ServerHandler::removeChannel(unsigned int channel) {
	MumbleProto::ChannelRemove mpcr;
	mpcr.set_channel_id(channel);
	sendMessage(mpcr);
}

void ServerHandler::addChannelLink(unsigned int channel, unsigned int link) {
	MumbleProto::ChannelState mpcs;
	mpcs.set_channel_id(channel);
	mpcs.add_links_add(link);
	sendMessage(mpcs);
}

void ServerHandler::removeChannelLink(unsigned int channel, unsigned int link) {
	MumbleProto::ChannelState mpcs;
	mpcs.set_channel_id(channel);
	mpcs.add_links_remove(link);
	sendMessage(mpcs);
}

void ServerHandler::requestChannelPermissions(unsigned int channel) {
	MumbleProto::PermissionQuery mppq;
	mppq.set_channel_id(channel);
	sendMessage(mppq);
}

void ServerHandler::setSelfMuteDeafState(bool mute, bool deaf) {
	MumbleProto::UserState mpus;
	mpus.set_self_mute(mute);
	mpus.set_self_deaf(deaf);
	sendMessage(mpus);
}

void ServerHandler::announceRecordingState(bool recording) {
	MumbleProto::UserState mpus;
	mpus.set_recording(recording);
	sendMessage(mpus);
}

QUrl ServerHandler::getServerURL(bool withPassword) const {
	QUrl url;

	url.setScheme(QLatin1String("mumble"));
	url.setHost(qsHostName);
	if (usPort != DEFAULT_MUMBLE_PORT) {
		url.setPort(usPort);
	}

	url.setUserName(qsUserName);

	if (withPassword && !qsPassword.isEmpty()) {
		url.setPassword(qsPassword);
	}

	return url;
}
