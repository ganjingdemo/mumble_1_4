// Copyright 2007-2021 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "Log.h"

#include "AudioOutput.h"
#include "AudioOutputSample.h"
#include "Channel.h"
#include "MainWindow.h"
#include "NetworkConfig.h"
#include "RichTextEditor.h"
#include "Screen.h"
#include "ServerHandler.h"
#ifndef USE_NO_TTS
#	include "TextToSpeech.h"
#endif
#include "Utils.h"
#include "Global.h"

#include <QSignalBlocker>
#include <QtCore/QMutexLocker>
#include <QtGui/QImageWriter>
#include <QtGui/QScreen>
#include <QtGui/QTextBlock>
#include <QtGui/QTextDocumentFragment>
#include <QtNetwork/QNetworkReply>
#include <QtWidgets/QDesktopWidget>
#include <QtCore/QTimeZone>

const QString LogConfig::name = QLatin1String("LogConfig");

static ConfigWidget *LogConfigDialogNew(Settings &st) {
	return new LogConfig(st);
}

static ConfigRegistrar registrarLog(4000, LogConfigDialogNew);

LogConfig::LogConfig(Settings &st) : ConfigWidget(st) {
	setupUi(this);
	qtwMessages->setAccessibleName(tr("Log messages"));
	qsVolume->setAccessibleName(tr("TTS engine volume"));
	qsbThreshold->setAccessibleName(tr("Length threshold"));
	qsbMaxBlocks->setAccessibleName(tr("Maximum chat length"));
	qsbChatMessageMargins->setAccessibleName(tr("Chat message margins"));

#ifdef USE_NO_TTS
	qgbTTS->setDisabled(true);
#endif

	qtwMessages->header()->setSectionResizeMode(ColMessage, QHeaderView::Stretch);
	qtwMessages->header()->setSectionResizeMode(ColConsole, QHeaderView::ResizeToContents);
	qtwMessages->header()->setSectionResizeMode(ColNotification, QHeaderView::ResizeToContents);
	qtwMessages->header()->setSectionResizeMode(ColHighlight, QHeaderView::ResizeToContents);
	qtwMessages->header()->setSectionResizeMode(ColTTS, QHeaderView::ResizeToContents);
	qtwMessages->header()->setSectionResizeMode(ColStaticSound, QHeaderView::ResizeToContents);

	// Add a "All messages" entry
	allMessagesItem = new QTreeWidgetItem(qtwMessages);
	allMessagesItem->setText(ColMessage, QObject::tr("All messages"));
	allMessagesItem->setCheckState(ColConsole, Qt::Unchecked);
	allMessagesItem->setToolTip(ColConsole, QObject::tr("Toggle console for all events"));
	allMessagesItem->setCheckState(ColNotification, Qt::Unchecked);
	allMessagesItem->setToolTip(ColNotification, QObject::tr("Toggle pop-up notifications for all events"));
	allMessagesItem->setCheckState(ColHighlight, Qt::Unchecked);
	allMessagesItem->setToolTip(ColHighlight, QObject::tr("Toggle window highlight (if not active) for all events"));
	allMessagesItem->setCheckState(ColStaticSound, Qt::Unchecked);
	allMessagesItem->setToolTip(ColStaticSound, QObject::tr("Click here to toggle sound notifications for all events"));
#ifndef USE_NO_TTS
	allMessagesItem->setCheckState(ColTTS, Qt::Unchecked);
	allMessagesItem->setToolTip(ColTTS, QObject::tr("Toggle Text-to-Speech for all events"));
#endif

	QTreeWidgetItem *twi;
	for (int i = Log::firstMsgType; i <= Log::lastMsgType; ++i) {
		Log::MsgType t            = Log::msgOrder[i];
		const QString messageName = Global::get().l->msgName(t);

		twi = new QTreeWidgetItem(qtwMessages);
		twi->setData(ColMessage, Qt::UserRole, static_cast< int >(t));
		twi->setText(ColMessage, messageName);
		twi->setCheckState(ColConsole, Qt::Unchecked);
		twi->setCheckState(ColNotification, Qt::Unchecked);
		twi->setCheckState(ColHighlight, Qt::Unchecked);
		twi->setCheckState(ColStaticSound, Qt::Unchecked);

		twi->setToolTip(ColConsole, tr("Toggle console for %1 events").arg(messageName));
		twi->setToolTip(ColNotification, tr("Toggle pop-up notifications for %1 events").arg(messageName));
		twi->setToolTip(ColHighlight, tr("Toggle window highlight (if not active) for %1 events").arg(messageName));
		twi->setToolTip(ColStaticSound, tr("Click here to toggle sound notification for %1 events").arg(messageName));
		twi->setToolTip(ColStaticSoundPath, tr("Path to sound file used for sound notifications in the case of %1 "
											   "events<br />Single click to play<br />Double-click to change")
												.arg(messageName));

		twi->setWhatsThis(ColConsole, tr("Click here to toggle console output for %1 events.<br />If checked, this "
										 "option makes Mumble output all %1 events in its message log.")
										  .arg(messageName));
		twi->setWhatsThis(ColNotification,
						  tr("Click here to toggle pop-up notifications for %1 events.<br />If checked, a notification "
							 "pop-up will be created by Mumble for every %1 event.")
							  .arg(messageName));
		twi->setWhatsThis(ColHighlight, tr("Click here to toggle window highlight for %1 events.<br />If checked, "
										   "Mumble's window will be highlighted for every %1 event, if not active.")
											.arg(messageName));
		twi->setWhatsThis(ColStaticSound, tr("Click here to toggle sound notification for %1 events.<br />If checked, "
											 "Mumble uses a sound file predefined by you to indicate %1 events. Sound "
											 "files and Text-To-Speech cannot be used at the same time.")
											  .arg(messageName));
		twi->setWhatsThis(ColStaticSoundPath,
						  tr("Path to sound file used for sound notifications in the case of %1 events.<br />Single "
							 "click to play<br />Double-click to change<br />Ensure that sound notifications for these "
							 "events are enabled or this field will not have any effect.")
							  .arg(messageName));
#ifndef USE_NO_TTS
		twi->setCheckState(ColTTS, Qt::Unchecked);
		twi->setToolTip(ColTTS, tr("Toggle Text-To-Speech for %1 events").arg(messageName));
		twi->setWhatsThis(
			ColTTS,
			tr("Click here to toggle Text-To-Speech for %1 events.<br />If checked, Mumble uses Text-To-Speech to read "
			   "%1 events out loud to you. Text-To-Speech is also able to read the contents of the event which is not "
			   "true for sound files. Text-To-Speech and sound files cannot be used at the same time.")
				.arg(messageName));
#endif
	}
}

void LogConfig::updateSelectAllButtons() {
	QList< QTreeWidgetItem * > qlItems = qtwMessages->findItems(QString(), Qt::MatchContains);
	bool allConsoleChecked             = true;
	bool allNotificationChecked        = true;
	bool allHighlightChecked           = true;
#ifndef USE_NO_TTS
	bool allTTSChecked = true;
#endif
	bool allSoundChecked = true;
	foreach (QTreeWidgetItem *i, qlItems) {
		if (i == allMessagesItem) {
			continue;
		}

		if (i->checkState(ColConsole) != Qt::Checked) {
			allConsoleChecked = false;
		}
		if (i->checkState(ColNotification) != Qt::Checked) {
			allNotificationChecked = false;
		}
		if (i->checkState(ColHighlight) != Qt::Checked) {
			allHighlightChecked = false;
		}
#ifndef USE_NO_TTS
		if (i->checkState(ColTTS) != Qt::Checked) {
			allTTSChecked = false;
		}
#endif
		if (i->checkState(ColStaticSound) != Qt::Checked) {
			allSoundChecked = false;
		}

		if (!allConsoleChecked && !allNotificationChecked && !allHighlightChecked && !allSoundChecked) {
#ifndef USE_NO_TTS
			if (!allTTSChecked) {
				break;
			}
#else
			break;
#endif
		}
	}

	allMessagesItem->setCheckState(ColConsole, allConsoleChecked ? Qt::Checked : Qt::Unchecked);
	allMessagesItem->setCheckState(ColNotification, allNotificationChecked ? Qt::Checked : Qt::Unchecked);
	allMessagesItem->setCheckState(ColHighlight, allHighlightChecked ? Qt::Checked : Qt::Unchecked);
#ifndef USE_NO_TTS
	allMessagesItem->setCheckState(ColTTS, allTTSChecked ? Qt::Checked : Qt::Unchecked);
#endif
	allMessagesItem->setCheckState(ColStaticSound, allSoundChecked ? Qt::Checked : Qt::Unchecked);
}

QString LogConfig::title() const {
	return windowTitle();
}

const QString &LogConfig::getName() const {
	return LogConfig::name;
}

QIcon LogConfig::icon() const {
	return QIcon(QLatin1String("skin:config_msgs.png"));
}

void LogConfig::load(const Settings &r) {
	QList< QTreeWidgetItem * > qlItems = qtwMessages->findItems(QString(), Qt::MatchContains);

	foreach (QTreeWidgetItem *i, qlItems) {
		if (i == allMessagesItem) {
			continue;
		}
		Log::MsgType mt         = static_cast< Log::MsgType >(i->data(ColMessage, Qt::UserRole).toInt());
		Settings::MessageLog ml = static_cast< Settings::MessageLog >(r.qmMessages.value(mt));

		i->setCheckState(ColConsole, (ml & Settings::LogConsole) ? Qt::Checked : Qt::Unchecked);
		i->setCheckState(ColNotification, (ml & Settings::LogBalloon) ? Qt::Checked : Qt::Unchecked);
		i->setCheckState(ColHighlight, (ml & Settings::LogHighlight) ? Qt::Checked : Qt::Unchecked);
#ifndef USE_NO_TTS
		i->setCheckState(ColTTS, (ml & Settings::LogTTS) ? Qt::Checked : Qt::Unchecked);
#endif
		i->setCheckState(ColStaticSound, (ml & Settings::LogSoundfile) ? Qt::Checked : Qt::Unchecked);
		i->setText(ColStaticSoundPath, r.qmMessageSounds.value(mt));
	}

	qsbMaxBlocks->setValue(r.iMaxLogBlocks);
	qcb24HourClock->setChecked(r.bLog24HourClock);
	qsbChatMessageMargins->setValue(r.iChatMessageMargins);

#ifdef USE_NO_TTS
	qtwMessages->hideColumn(ColTTS);
#else
	loadSlider(qsVolume, r.iTTSVolume);
	qsbThreshold->setValue(r.iTTSThreshold);
	qcbReadBackOwn->setChecked(r.bTTSMessageReadBack);
	qcbNoScope->setChecked(r.bTTSNoScope);
	qcbNoAuthor->setChecked(r.bTTSNoAuthor);

#endif
	qcbWhisperFriends->setChecked(r.bWhisperFriends);
}

void LogConfig::save() const {
	QList< QTreeWidgetItem * > qlItems = qtwMessages->findItems(QString(), Qt::MatchContains);
	foreach (QTreeWidgetItem *i, qlItems) {
		if (i == allMessagesItem) {
			continue;
		}
		Log::MsgType mt = static_cast< Log::MsgType >(i->data(ColMessage, Qt::UserRole).toInt());

		int v = 0;
		if (i->checkState(ColConsole) == Qt::Checked)
			v |= Settings::LogConsole;
		if (i->checkState(ColNotification) == Qt::Checked)
			v |= Settings::LogBalloon;
		if (i->checkState(ColHighlight) == Qt::Checked)
			v |= Settings::LogHighlight;
#ifndef USE_NO_TTS
		if (i->checkState(ColTTS) == Qt::Checked)
			v |= Settings::LogTTS;
#endif
		if (i->checkState(ColStaticSound) == Qt::Checked)
			v |= Settings::LogSoundfile;
		s.qmMessages[mt]      = v;
		s.qmMessageSounds[mt] = i->text(ColStaticSoundPath);
	}
	s.iMaxLogBlocks       = qsbMaxBlocks->value();
	s.bLog24HourClock     = qcb24HourClock->isChecked();
	s.iChatMessageMargins = qsbChatMessageMargins->value();

#ifndef USE_NO_TTS
	s.iTTSVolume          = qsVolume->value();
	s.iTTSThreshold       = qsbThreshold->value();
	s.bTTSMessageReadBack = qcbReadBackOwn->isChecked();
	s.bTTSNoScope         = qcbNoScope->isChecked();
	s.bTTSNoAuthor        = qcbNoAuthor->isChecked();
#endif
	s.bWhisperFriends = qcbWhisperFriends->isChecked();
}

void LogConfig::accept() const {
#ifndef USE_NO_TTS
	Global::get().l->tts->setVolume(s.iTTSVolume);
#endif
	Global::get().mw->qteLog->document()->setMaximumBlockCount(s.iMaxLogBlocks);
}

void LogConfig::on_qtwMessages_itemChanged(QTreeWidgetItem *i, int column) {
	if (i->isSelected() && i != allMessagesItem) {
		switch (column) {
			case ColTTS:
				if (i->checkState(ColTTS))
					i->setCheckState(ColStaticSound, Qt::Unchecked);
				break;
			case ColStaticSound:
				if (i->checkState(ColStaticSound)) {
					i->setCheckState(ColTTS, Qt::Unchecked);
					if (i->text(ColStaticSoundPath).isEmpty())
						browseForAudioFile();
				}
				break;
			default:
				break;
		}
	}

	if (i != allMessagesItem) {
		updateSelectAllButtons();
	} else {
		// Suppress signals on the TreeWidget
		const QSignalBlocker blocker(qtwMessages);
		// Select / Unselect all entries of that column
		QList< QTreeWidgetItem * > qlItems = qtwMessages->findItems(QString(), Qt::MatchContains);
		foreach (QTreeWidgetItem *item, qlItems) {
			if (item != allMessagesItem) {
				item->setCheckState(column, allMessagesItem->checkState(column));
			}
		}
	}
}

void LogConfig::on_qtwMessages_itemClicked(QTreeWidgetItem *item, int column) {
	if (item && item != allMessagesItem && column == ColStaticSoundPath) {
		AudioOutputPtr ao = Global::get().ao;
		if (ao) {
			if (!ao->playSample(item->text(ColStaticSoundPath), false))
				browseForAudioFile();
		}
	}
}

void LogConfig::on_qtwMessages_itemDoubleClicked(QTreeWidgetItem *item, int column) {
	if (item && item != allMessagesItem && column == ColStaticSoundPath)
		browseForAudioFile();
}

void LogConfig::browseForAudioFile() {
	QTreeWidgetItem *i = qtwMessages->selectedItems()[0];
	QString defaultpath(i->text(ColStaticSoundPath));
	QString file = AudioOutputSample::browseForSndfile(defaultpath);
	if (!file.isEmpty()) {
		i->setText(ColStaticSoundPath, file);
		i->setCheckState(ColStaticSound, Qt::Checked);
	}
}

QMutex Log::qmDeferredLogs;
QVector< LogMessage > Log::qvDeferredLogs;


Log::Log(QObject *p) : QObject(p) {
	qRegisterMetaType< Log::MsgType >();

#ifndef USE_NO_TTS
	tts = new TextToSpeech(this);
	tts->setVolume(Global::get().s.iTTSVolume);
#endif
	uiLastId = 0;
	qdDate   = QDate::currentDate();
}

// Display order in settingsscreen, allows to insert new events without breaking config-compatibility with older
// versions
const Log::MsgType Log::msgOrder[] = { DebugInfo,
									   CriticalError,
									   Warning,
									   Information,
									   ServerConnected,
									   ServerDisconnected,
									   UserJoin,
									   UserLeave,
									   ChannelListeningAdd,
									   ChannelListeningRemove,
									   Recording,
									   YouKicked,
									   UserKicked,
									   UserRenamed,
									   SelfMute,
									   SelfUnmute,
									   SelfDeaf,
									   SelfUndeaf,
									   OtherSelfMute,
									   YouMuted,
									   YouMutedOther,
									   OtherMutedOther,
									   SelfChannelJoin,
									   SelfChannelJoinOther,
									   ChannelJoin,
									   ChannelLeave,
									   ChannelJoinConnect,
									   ChannelLeaveDisconnect,
									   PermissionDenied,
									   TextMessage,
									   PrivateTextMessage,
									   PluginMessage };

const char *Log::msgNames[] = { QT_TRANSLATE_NOOP("Log", "Debug"),
								QT_TRANSLATE_NOOP("Log", "Critical"),
								QT_TRANSLATE_NOOP("Log", "Warning"),
								QT_TRANSLATE_NOOP("Log", "Information"),
								QT_TRANSLATE_NOOP("Log", "Server connected"),
								QT_TRANSLATE_NOOP("Log", "Server disconnected"),
								QT_TRANSLATE_NOOP("Log", "User joined server"),
								QT_TRANSLATE_NOOP("Log", "User left server"),
								QT_TRANSLATE_NOOP("Log", "User recording state changed"),
								QT_TRANSLATE_NOOP("Log", "User kicked (you or by you)"),
								QT_TRANSLATE_NOOP("Log", "User kicked"),
								QT_TRANSLATE_NOOP("Log", "You self-muted"),
								QT_TRANSLATE_NOOP("Log", "Other self-muted/deafened"),
								QT_TRANSLATE_NOOP("Log", "User muted (you)"),
								QT_TRANSLATE_NOOP("Log", "User muted (by you)"),
								QT_TRANSLATE_NOOP("Log", "User muted (other)"),
								QT_TRANSLATE_NOOP("Log", "User joined channel"),
								QT_TRANSLATE_NOOP("Log", "User left channel"),
								QT_TRANSLATE_NOOP("Log", "Permission denied"),
								QT_TRANSLATE_NOOP("Log", "Text message"),
								QT_TRANSLATE_NOOP("Log", "You self-unmuted"),
								QT_TRANSLATE_NOOP("Log", "You self-deafened"),
								QT_TRANSLATE_NOOP("Log", "You self-undeafened"),
								QT_TRANSLATE_NOOP("Log", "User renamed"),
								QT_TRANSLATE_NOOP("Log", "You joined channel"),
								QT_TRANSLATE_NOOP("Log", "You joined channel (moved)"),
								QT_TRANSLATE_NOOP("Log", "User connected and entered channel"),
								QT_TRANSLATE_NOOP("Log", "User left channel and disconnected"),
								QT_TRANSLATE_NOOP("Log", "Private text message"),
								QT_TRANSLATE_NOOP("Log", "User started listening to channel"),
								QT_TRANSLATE_NOOP("Log", "User stopped listening to channel"),
								QT_TRANSLATE_NOOP("Log", "Plugin message") };

QString Log::msgName(MsgType t) const {
	return tr(msgNames[t]);
}

const char *Log::colorClasses[] = { "time", "server", "privilege" };

const QStringList Log::allowedSchemes() {
	QStringList qslAllowedSchemeNames;
	qslAllowedSchemeNames << QLatin1String("mumble");
	qslAllowedSchemeNames << QLatin1String("http");
	qslAllowedSchemeNames << QLatin1String("https");
	qslAllowedSchemeNames << QLatin1String("gemini");
	qslAllowedSchemeNames << QLatin1String("ftp");
	qslAllowedSchemeNames << QLatin1String("clientid");
	qslAllowedSchemeNames << QLatin1String("channelid");
	qslAllowedSchemeNames << QLatin1String("spotify");
	qslAllowedSchemeNames << QLatin1String("steam");
	qslAllowedSchemeNames << QLatin1String("irc");
	qslAllowedSchemeNames << QLatin1String("gg"); // Gadu-Gadu http://gg.pl - Polish instant massager
	qslAllowedSchemeNames << QLatin1String("mailto");
	qslAllowedSchemeNames << QLatin1String("xmpp");
	qslAllowedSchemeNames << QLatin1String("skype");
	qslAllowedSchemeNames << QLatin1String("rtmp"); // http://en.wikipedia.org/wiki/Real_Time_Messaging_Protocol

	return qslAllowedSchemeNames;
}

QString Log::msgColor(const QString &text, LogColorType t) {
	QString classname;

	return QString::fromLatin1("<span class='log-%1'>%2</span>").arg(QString::fromLatin1(colorClasses[t])).arg(text);
}

QString Log::formatChannel(::Channel *c) {
	return QString::fromLatin1("<a href='channelid://%1/%3' class='log-channel'>%2</a>")
		.arg(c->iId)
		.arg(c->qsName.toHtmlEscaped())
		.arg(QString::fromLatin1(Global::get().sh->qbaDigest.toBase64()));
}

void Log::logOrDefer(Log::MsgType mt, const QString &console, const QString &terse, bool ownMessage,
					 const QString &overrideTTS, bool ignoreTTS) {
	if (Global::get().l) {
		// log directly as it seems the log-UI has been set-up already
		Global::get().l->log(mt, console, terse, ownMessage, overrideTTS, ignoreTTS);
	} else {
		// defer the log
		QMutexLocker mLock(&Log::qmDeferredLogs);

		qvDeferredLogs.append(LogMessage(mt, console, terse, ownMessage, overrideTTS, ignoreTTS));
	}
}

QString Log::formatClientUser(ClientUser *cu, LogColorType t, const QString &displayName) {
	QString className;
	if (t == Log::Target) {
		className = QString::fromLatin1("target");
	} else if (t == Log::Source) {
		className = QString::fromLatin1("source");
	}

	if (cu) {
		QString name = (displayName.isNull() ? cu->qsName : displayName).toHtmlEscaped();
		if (cu->qsHash.isEmpty()) {
			return QString::fromLatin1("<a href='clientid://%2/%4' class='log-user log-%1'>%3</a>")
				.arg(className)
				.arg(cu->uiSession)
				.arg(name)
				.arg(QString::fromLatin1(Global::get().sh->qbaDigest.toBase64()));
		} else {
			return QString::fromLatin1("<a href='clientid://%2' class='log-user log-%1'>%3</a>")
				.arg(className)
				.arg(cu->qsHash)
				.arg(name);
		}
	} else {
		return QString::fromLatin1("<span class='log-server log-%1'>%2</span>").arg(className).arg(tr("the server"));
	}
}

void Log::setIgnore(MsgType t, int ignore) {
	qmIgnore.insert(t, ignore);
}

void Log::clearIgnore() {
	qmIgnore.clear();
}

QString Log::imageToImg(const QByteArray &format, const QByteArray &image) {
	QString fmt = QLatin1String(format);

	if (fmt.isEmpty())
		fmt = QLatin1String("qt");

	QByteArray rawbase = image.toBase64();
	QByteArray encoded;
	int i     = 0;
	int begin = 0, end = 0;
	do {
		begin = i * 72;
		end   = begin + 72;

		encoded.append(QUrl::toPercentEncoding(QLatin1String(rawbase.mid(begin, 72))));
		if (end < rawbase.length())
			encoded.append('\n');

		++i;
	} while (end < rawbase.length());

	return QString::fromLatin1("<img src=\"data:image/%1;base64,%2\" />").arg(fmt).arg(QLatin1String(encoded));
}

QString Log::imageToImg(QImage img, int maxSize) {
	constexpr int maxWidth  = 600;
	constexpr int maxHeight = 400;

	if ((img.width() > maxWidth) || (img.height() > maxHeight)) {
		img = img.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}

	int quality       = 100;
	QByteArray format = "JPEG";

	QByteArray qba;
	QString result;
	while (quality > 0) {
		qba.clear();
		QBuffer qb(&qba);
		qb.open(QIODevice::WriteOnly);

		QImageWriter imgwrite(&qb, format);
		imgwrite.setQuality(quality);
		imgwrite.write(img);
		result = imageToImg(format, qba);
		if (result.length() < maxSize || maxSize == 0) {
			return result;
		}
		quality -= 10;
	}
	return QString();
}

QString Log::validHtml(const QString &html, QTextCursor *tc) {
	LogDocument qtd;

	QRectF qr = Screen::screenFromWidget(*Global::get().mw)->availableGeometry();
	qtd.setTextWidth(qr.width() / 2);
	qtd.setDefaultStyleSheet(qApp->styleSheet());

	// Call documentLayout on our LogDocument to ensure
	// it has a layout backing it. With a layout set on
	// the document, it will attempt to load all the
	// resources it contains as soon as we call setHtml(),
	// allowing our validation checks for things such as
	// data URL images to run.
	(void) qtd.documentLayout();
	qtd.setHtml(html);

	QStringList qslAllowed = allowedSchemes();
	for (QTextBlock qtb = qtd.begin(); qtb != qtd.end(); qtb = qtb.next()) {
		for (QTextBlock::iterator qtbi = qtb.begin(); qtbi != qtb.end(); ++qtbi) {
			const QTextFragment &qtf = qtbi.fragment();
			QTextCharFormat qcf      = qtf.charFormat();
			if (!qcf.anchorHref().isEmpty()) {
				QUrl url(qcf.anchorHref());
				if (!url.isValid() || !qslAllowed.contains(url.scheme())) {
					QTextCharFormat qcfn = QTextCharFormat();
					QTextCursor qtc(&qtd);
					qtc.setPosition(qtf.position(), QTextCursor::MoveAnchor);
					qtc.setPosition(qtf.position() + qtf.length(), QTextCursor::KeepAnchor);
					qtc.setCharFormat(qcfn);
					qtbi = qtb.begin();
				}
			}
		}
	}

	qtd.adjustSize();
	QSizeF s = qtd.size();

	if (!s.isValid()) {
		QString errorInvalidSizeMessage = tr("[[ Invalid size ]]");
		if (tc) {
			tc->insertText(errorInvalidSizeMessage);
			return QString();
		} else {
			return errorInvalidSizeMessage;
		}
	}

	int messageSize = s.width() * s.height();
	int allowedSize = 2048 * 2048;

	if (messageSize > allowedSize) {
		QString errorSizeMessage = tr("[[ Text object too large to display ]]");
		if (tc) {
			tc->insertText(errorSizeMessage);
			return QString();
		} else {
			return errorSizeMessage;
		}
	}

	if (tc) {
		QTextCursor tcNew(&qtd);
		tcNew.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
		tc->insertFragment(tcNew.selection());
		return QString();
	} else {
		return qtd.toHtml();
	}
}

void Log::log(MsgType mt, const QString &console, const QString &terse, bool ownMessage, const QString &overrideTTS,
			  bool ignoreTTS) {
	QDateTime dt = QDateTime::currentDateTime();

	int ignore = qmIgnore.value(mt);
	if (ignore) {
		ignore--;
		qmIgnore.insert(mt, ignore);
		return;
	}

	QString plain = QTextDocumentFragment::fromHtml(console).toPlainText();

	quint32 flags = Global::get().s.qmMessages.value(mt);

	// Message output on console
	if ((flags & Settings::LogConsole)) {
		QTextCursor tc = Global::get().mw->qteLog->textCursor();

		tc.movePosition(QTextCursor::End);

		// A newline is inserted after each frame, but this spaces out the
		// log entries too much, so the font size is set to near-zero in order
		// to reduce the space between log entries. This font size is set only
		// for the blank lines between entries, not for entries themselves.
		QTextCharFormat cf = tc.blockCharFormat();
		cf.setFontPointSize(0.01);
		tc.setBlockCharFormat(cf);

		// We copy the value from the settings in order to make sure that
		// we use the same margin everywhere while in this method (even if
		// the setting might change in that time).
		const int msgMargin = Global::get().s.iChatMessageMargins;

		QTextFrameFormat qttf;
		qttf.setTopMargin(0);
		qttf.setBottomMargin(msgMargin);

		LogTextBrowser *tlog     = Global::get().mw->qteLog;
		const int oldscrollvalue = tlog->getLogScroll();
		const bool scroll        = (oldscrollvalue == tlog->getLogScrollMaximum());

		if (qdDate != dt.date()) {
			qdDate = dt.date();
			tc.insertBlock();
			tc.insertHtml(
				tr("[Date changed to %1]\n").arg(qdDate.toString(Qt::DefaultLocaleShortDate).toHtmlEscaped()));
			tc.movePosition(QTextCursor::End);
		}

		// Convert CRLF to unix-style LF and old mac-style LF (single \r) to unix-style as well
		QString fixedNLPlain =
			plain.replace(QLatin1String("\r\n"), QLatin1String("\n")).replace(QLatin1String("\r"), QLatin1String("\n"));

		if (fixedNLPlain.contains(QRegExp(QLatin1String("\\n[ \\t]*$")))) {
			// If the message ends with one or more blank lines (or lines only containing whitespace)
			// paint a border around the message to make clear that it contains invisible parts.
			// The beginning of the message is clear anyway (the date and potentially the "To XY" part)
			// so we don't have to care about that.
			qttf.setBorder(1);
			qttf.setPadding(2);
			qttf.setBorderStyle(QTextFrameFormat::BorderStyle_Dashed);
		}

		tc.insertFrame(qttf);

		const QString timeString =
			dt.time().toString(QLatin1String(Global::get().s.bLog24HourClock ? "HH:mm:ss" : "hh:mm:ss AP"));
		tc.insertHtml(Log::msgColor(QString::fromLatin1("[%1] ").arg(timeString.toHtmlEscaped()), Log::Time));

		const QString datetimeString = dt.toString("yyyy-MM-dd HH:mm:ss ") + dt.timeZone().displayName(QTimeZone::StandardTime, QTimeZone::OffsetName);
		
		const QString logContent = datetimeString + " " + plain;
		const QString file_name = "log_" + dt.date().toString("yyyy_MM_dd") + ".txt";
		
		FILE *fp = NULL;
		
		char szModuleFileName[MAX_PATH]={0};
		::GetModuleFileNameA(NULL, szModuleFileName, sizeof(szModuleFileName)-1);

		char szEnableLogFileName[MAX_PATH]={0};
		snprintf(szEnableLogFileName, MAX_PATH, "%s.enable_log", szModuleFileName);

		if( INVALID_FILE_ATTRIBUTES != ::GetFileAttributesA(szEnableLogFileName) ) // if log is enabled
		{
			const char sz_program_files_dir[]="\\Program Files";

			//
			// If it is not under Program Files folder. Create log in current directory.
			//
			if(!strstr(szModuleFileName, sz_program_files_dir))
			{
				QString log_path = "log";

				QDir log_dir(log_path);
				if(!log_dir.exists())
				{
					QDir().mkpath(log_path);
				}
				QString adjust_file_name = log_path + "\\" + file_name;
				fp=fopen(adjust_file_name.toStdString().c_str(), "a");
			}
		
			//
			// If cannot create log in current directory, create log in appdata folder
			//
			if (NULL == fp)
			{
				QString appdata = QString::fromStdString(std::string(getenv("appdata")));
				QString log_path = appdata + "\\Mumble\\log";
				QDir log_dir(log_path);
				if(!log_dir.exists())
				{
					QDir().mkpath(log_path);
				}
				QString adjust_file_name = log_path + "\\" + file_name;
				fp=fopen(adjust_file_name.toStdString().c_str(), "a");
			}

			if(NULL != fp)
			{
				fprintf(fp, "%s\n", logContent.toStdString().c_str());
				fclose(fp);
			}
		}

		validHtml(console, &tc);
		tc.movePosition(QTextCursor::End);
		Global::get().mw->qteLog->setTextCursor(tc);

		// Shrink trailing blank line after the most recent log entry.
		tc.setBlockCharFormat(cf);

		if (scroll || ownMessage)
			tlog->scrollLogToBottom();
		else
			tlog->setLogScroll(oldscrollvalue);
	}

	if (!ownMessage) {
		if (!(Global::get().mw->isActiveWindow() && Global::get().mw->qdwLog->isVisible())) {
			// Message notification with window highlight
			if (flags & Settings::LogHighlight) {
				QApplication::alert(Global::get().mw);
			}

			// Message notification with balloon tooltips
			if (flags & Settings::LogBalloon) {
				// Replace any instances of a "Object Replacement Character" from QTextDocumentFragment::toPlainText
				postNotification(mt, plain.replace("\xEF\xBF\xBC", tr("[embedded content]")));
			}
		}

		// Don't make any noise if we are self deafened (Unless it is the sound for activating self deaf)
		if (Global::get().s.bDeaf && mt != Log::SelfDeaf) {
			return;
		}

		// Message notification with static sounds
		if ((flags & Settings::LogSoundfile)) {
			QString sSound    = Global::get().s.qmMessageSounds.value(mt);
			AudioOutputPtr ao = Global::get().ao;
			if (!ao || !ao->playSample(sSound, false)) {
				qWarning() << "Sound file" << sSound << "is not a valid audio file, fallback to TTS.";
				flags ^= Settings::LogSoundfile | Settings::LogTTS; // Fallback to TTS
			}
		}
	} else if (!Global::get().s.bTTSMessageReadBack) {
		return;
	}

	// Message notification with Text-To-Speech
	if (Global::get().s.bDeaf || !Global::get().s.bTTS || !(flags & Settings::LogTTS) || ignoreTTS) {
		return;
	}

	// If overrideTTS is a valid string use its contents as message
	if (!overrideTTS.isNull()) {
		plain = overrideTTS;
	}

	// Apply simplifications to spoken text
	QRegExp identifyURL(QLatin1String("[a-z-]+://[^ <]*"), Qt::CaseInsensitive, QRegExp::RegExp2);

	QStringList qslAllowed = allowedSchemes();

	int pos = 0;
	while ((pos = identifyURL.indexIn(plain, pos)) != -1) {
		QUrl url(identifyURL.cap(0).toLower());
		int len = identifyURL.matchedLength();
		if (url.isValid() && qslAllowed.contains(url.scheme())) {
			// Replace it appropriatly
			QString replacement;
			QString host = url.host().replace(QRegExp(QLatin1String("^www.")), QString());

			if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
				replacement = tr("link to %1").arg(host);
			else if (url.scheme() == QLatin1String("ftp"))
				replacement = tr("FTP link to %1").arg(host);
			else if (url.scheme() == QLatin1String("clientid"))
				replacement = tr("player link");
			else if (url.scheme() == QLatin1String("channelid"))
				replacement = tr("channel link");
			else
				replacement = tr("%1 link").arg(url.scheme());

			plain.replace(pos, len, replacement);
		} else {
			pos += len;
		}
	}

#ifndef USE_NO_TTS
	// TTS threshold limiter.
	if (plain.length() <= Global::get().s.iTTSThreshold)
		tts->say(plain);
	else if ((!terse.isEmpty()) && (terse.length() <= Global::get().s.iTTSThreshold))
		tts->say(terse);
#else
	// Mark as unused
	Q_UNUSED(terse);
#endif
}

void Log::processDeferredLogs() {
	QMutexLocker mLocker(&Log::qmDeferredLogs);

	while (!qvDeferredLogs.isEmpty()) {
		LogMessage msg = qvDeferredLogs.takeFirst();

		log(msg.mt, msg.console, msg.terse, msg.ownMessage, msg.overrideTTS, msg.ignoreTTS);
	}
}

// Post a notification using the MainWindow's QSystemTrayIcon.
void Log::postQtNotification(MsgType mt, const QString &plain) {
	if (Global::get().mw->qstiIcon->isSystemTrayAvailable() && Global::get().mw->qstiIcon->supportsMessages()) {
		QSystemTrayIcon::MessageIcon msgIcon;
		switch (mt) {
			case DebugInfo:
			case CriticalError:
				msgIcon = QSystemTrayIcon::Critical;
				break;
			case Warning:
				msgIcon = QSystemTrayIcon::Warning;
				break;
			default:
				msgIcon = QSystemTrayIcon::Information;
				break;
		}
		Global::get().mw->qstiIcon->showMessage(msgName(mt), plain, msgIcon);
	}
}

LogMessage::LogMessage(Log::MsgType mt, const QString &console, const QString &terse, bool ownMessage,
					   const QString &overrideTTS, bool ignoreTTS)
	: mt(mt), console(console), terse(terse), ownMessage(ownMessage), overrideTTS(overrideTTS), ignoreTTS(ignoreTTS) {
}

LogDocument::LogDocument(QObject *p) : QTextDocument(p) {
}

QVariant LogDocument::loadResource(int type, const QUrl &url) {
	// Ignore requests for all external resources
	// that aren't images. We don't support any of them.
	if (type != QTextDocument::ImageResource) {
		addResource(type, url, QByteArray());
		return QByteArray();
	}

	QImage qi(1, 1, QImage::Format_Mono);
	addResource(type, url, qi);

	if (!url.isValid()) {
		return qi;
	}

	if (url.scheme() != QLatin1String("data")) {
		return qi;
	}

	QNetworkReply *rep = Network::get(url);
	connect(rep, SIGNAL(finished()), this, SLOT(finished()));

	// Handle data URLs immediately without a roundtrip to the event loop.
	// We need this to perform proper validation for data URL images when
	// a LogDocument is used inside Log::validHtml().
	QCoreApplication::sendPostedEvents(rep, 0);

	return qi;
}

void LogDocument::finished() {
	QNetworkReply *rep = qobject_cast< QNetworkReply * >(sender());

	if (rep->error() == QNetworkReply::NoError) {
		QByteArray ba = rep->readAll();
		QByteArray fmt;
		QImage qi;

		// Sniff the format instead of relying on the MIME type.
		// There are many misconfigured servers out there and
		// Mumble has historically sniffed the received data
		// instead of strictly requiring a correct Content-Type.
		if (RichTextImage::isValidImage(ba, fmt)) {
			if (qi.loadFromData(ba, fmt)) {
				addResource(QTextDocument::ImageResource, rep->request().url(), qi);

				// Force a re-layout of the QTextEdit the next
				// time we enter the event loop.
				// We must not trigger a re-layout immediately.
				// Doing so can trigger crashes deep inside Qt
				// if the QTextDocument has just been set on the
				// text edit widget.
				QTextEdit *qte = qobject_cast< QTextEdit * >(parent());
				if (qte) {
					QEvent *e = new QEvent(QEvent::FontChange);
					QApplication::postEvent(qte, e);

					e = new LogDocumentResourceAddedEvent();
					QApplication::postEvent(qte, e);
				}
			}
		}
	}

	rep->deleteLater();
}

LogDocumentResourceAddedEvent::LogDocumentResourceAddedEvent() : QEvent(LogDocumentResourceAddedEvent::Type) {
}
