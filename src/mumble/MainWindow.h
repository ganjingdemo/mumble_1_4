// Copyright 2007-2021 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_MAINWINDOW_H_
#define MUMBLE_MUMBLE_MAINWINDOW_H_

#include <QtCore/QPointer>
#include <QtCore/QtGlobal>
#include <QtNetwork/QAbstractSocket>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>

#include "CustomElements.h"
#include "MUComboBox.h"
#include "Message.h"
#include "Mumble.pb.h"
#include "Usage.h"
#include "UserLocalNicknameDialog.h"
#include "UserLocalVolumeDialog.h"

#include "ui_MainWindow.h"

#define MB_QEVENT (QEvent::User + 939)
#define OU_QEVENT (QEvent::User + 940)

class ACLEditor;
class BanEditor;
class UserEdit;
class ServerHandler;
class GlobalShortcut;
class TextToSpeech;
class UserModel;
class Tokens;
class Channel;
class UserInformation;
class VoiceRecorderDialog;
class PTTButtonWidget;
namespace Search {
class SearchDialog;
};

struct ShortcutTarget;

struct ContextMenuTarget {
	ClientUser *user = nullptr;
	Channel *channel = nullptr;
};

class MessageBoxEvent : public QEvent {
public:
	QString msg;
	MessageBoxEvent(QString msg);
};

class OpenURLEvent : public QEvent {
public:
	QUrl url;
	OpenURLEvent(QUrl url);
};

class MainWindow : public QMainWindow, public MessageHandler, public Ui::MainWindow {
	friend class UserModel;

private:
	Q_OBJECT
	Q_DISABLE_COPY(MainWindow)
public:
	UserModel *pmModel;
	QSystemTrayIcon *qstiIcon;
	QMenu *qmUser;
	QMenu *qmChannel;
	QMenu *qmListener;
	QMenu *qmDeveloper;
	QMenu *qmTray;
	QIcon qiIcon, qiIconMutePushToMute, qiIconMuteSelf, qiIconMuteServer, qiIconDeafSelf, qiIconDeafServer,
		qiIconMuteSuppressed;
	QIcon qiTalkingOn, qiTalkingWhisper, qiTalkingShout, qiTalkingOff;
	QMap< unsigned int, UserLocalVolumeDialog * > qmUserVolTracker;
	std::unordered_map< unsigned int, NicknameDialogPtr > qmUserNicknameTracker;

	/// "Action" for when there are no actions available
	QAction *qaEmpty;

	GlobalShortcut *gsPushTalk, *gsResetAudio, *gsMuteSelf, *gsDeafSelf;
	GlobalShortcut *gsUnlink, *gsPushMute, *gsJoinChannel;
#ifdef USE_OVERLAY
	GlobalShortcut *gsToggleOverlay;
#endif
	GlobalShortcut *gsMinimal, *gsVolumeUp, *gsVolumeDown, *gsWhisper, *gsLinkChannel;
	GlobalShortcut *gsCycleTransmitMode, *gsToggleMainWindowVisibility, *gsTransmitModePushToTalk,
		*gsTransmitModeContinuous, *gsTransmitModeVAD;
	GlobalShortcut *gsSendTextMessage, *gsSendClipboardTextMessage;
	GlobalShortcut *gsToggleTalkingUI;
	GlobalShortcut *gsToggleSearch;

	DockTitleBar *dtbLogDockTitle, *dtbChatDockTitle;

	ACLEditor *aclEdit;
	BanEditor *banEdit;
	UserEdit *userEdit;
	Tokens *tokenEdit;

	VoiceRecorderDialog *voiceRecorderDialog;

	MumbleProto::Reject_RejectType rtLast;
	bool bRetryServer;
	QString qsDesiredChannel;

	bool bSuppressAskOnQuit;
	/// Restart the client after shutdown
	bool restartOnQuit;
	bool bAutoUnmute;

	/// Contains the cursor whose position is immediately before the image to
	/// save when activating the "Save Image As..." context menu item.
	QTextCursor qtcSaveImageCursor;

	QPointer< Channel > cContextChannel;
	QPointer< ClientUser > cuContextUser;

	QPoint qpContextPosition;

	void recheckTTS();
	void msgBox(QString msg);
	void setOnTop(bool top);
	void setShowDockTitleBars(bool doShow);
	void updateTrayIcon();
	void updateUserModel();
	void focusNextMainWidget();
	QPair< QByteArray, QImage > openImageFile();

	void updateChatBar();
	void openTextMessageDialog(ClientUser *p);
	void openUserLocalNicknameDialog(const ClientUser &p);
	void openUserLocalVolumeDialog(ClientUser *p);

#ifdef Q_OS_WIN
	bool nativeEvent(const QByteArray &eventType, void *message, long *result) Q_DECL_OVERRIDE;
	unsigned int uiNewHardware;
#endif
protected:
	Usage uUsage;
	QTimer *qtReconnect;

	QList< QAction * > qlServerActions;
	QList< QAction * > qlChannelActions;
	QList< QAction * > qlUserActions;

	QHash< ShortcutTarget, int > qmCurrentTargets;
	/// A map that contains information about the currently active
	/// shout/whisper targets. The mapping is between a List of
	/// ShortcutTargets that are all triggered together and the
	/// target ID for this specific combination of ShortcutTargets.
	/// The target ID is what the server uses to identify this specific
	/// set of ShortcutTargets.
	QHash< QList< ShortcutTarget >, int > qmTargets;
	/// This is a map between all target IDs the client will ever use
	/// and a helper-number (see iTargetCounter).
	QMap< int, int > qmTargetUse;
	Channel *mapChannel(int idx) const;
	/// This is a pure helper number whose job is to always be increased
	/// if a new VoiceTarget is needed. It will be used as the helper
	/// number in qmTargetUse.
	int iTargetCounter;
	QMap< unsigned int, UserInformation * > qmUserInformations;

	PTTButtonWidget *qwPTTButtonWidget;

	MUComboBox *qcbTransmitMode;
	QAction *qaTransmitMode;
	QAction *qaTransmitModeSeparator;

	Search::SearchDialog *m_searchDialog = nullptr;

	void createActions();
	void setupGui();
	void updateWindowTitle();
	/// updateToolbar updates the state of the toolbar depending on the current
	/// window layout setting.
	/// If the window layout setting is 'custom', the toolbar is made movable. If the
	/// window layout is not 'custom', the toolbar is locked in place at the top of
	/// the MainWindow.
	void updateToolbar();
	void customEvent(QEvent *evt) Q_DECL_OVERRIDE;
	void findDesiredChannel();
	void setupView(bool toggle_minimize = true);
	void closeEvent(QCloseEvent *e) Q_DECL_OVERRIDE;
	void hideEvent(QHideEvent *e) Q_DECL_OVERRIDE;
	void showEvent(QShowEvent *e) Q_DECL_OVERRIDE;
	void changeEvent(QEvent *e) Q_DECL_OVERRIDE;
	void keyPressEvent(QKeyEvent *e) Q_DECL_OVERRIDE;

	QMenu *createPopupMenu() Q_DECL_OVERRIDE;

	bool handleSpecialContextMenu(const QUrl &url, const QPoint &pos_, bool focus = false);
	Channel *getContextMenuChannel();
	ClientUser *getContextMenuUser();
	ContextMenuTarget getContextMenuTargets();

public slots:
	void on_qmServer_aboutToShow();
	void on_qaServerConnect_triggered(bool autoconnect = false);
	void on_qaServerDisconnect_triggered();
	void on_qaServerBanList_triggered();
	void on_qaServerUserList_triggered();
	void on_qaServerInformation_triggered();
	void on_qaServerTexture_triggered();
	void on_qaServerTextureRemove_triggered();
	void on_qaServerTokens_triggered();
	void on_qmSelf_aboutToShow();
	void on_qaSelfComment_triggered();
	void on_qaSelfRegister_triggered();
	void qcbTransmitMode_activated(int index);
	void updateTransmitModeComboBox(Settings::AudioTransmit newMode);
	void qmUser_aboutToShow();
	void qmListener_aboutToShow();
	void on_qaUserCommentReset_triggered();
	void on_qaUserTextureReset_triggered();
	void on_qaUserCommentView_triggered();
	void on_qaUserKick_triggered();
	void on_qaUserBan_triggered();
	void on_qaUserMute_triggered();
	void on_qaUserDeaf_triggered();
	void on_qaSelfPrioritySpeaker_triggered();
	void on_qaUserPrioritySpeaker_triggered();
	void on_qaUserLocalIgnore_triggered();
	void on_qaUserLocalIgnoreTTS_triggered();
	void on_qaUserLocalMute_triggered();
	void on_qaUserLocalNickname_triggered();
	void on_qaUserLocalVolume_triggered();
	void on_qaUserTextMessage_triggered();
	void on_qaUserRegister_triggered();
	void on_qaUserInformation_triggered();
	void on_qaUserFriendAdd_triggered();
	void on_qaUserFriendRemove_triggered();
	void on_qaUserFriendUpdate_triggered();
	void qmChannel_aboutToShow();
	void on_qaChannelJoin_triggered();
	void on_qaUserJoin_triggered();
	void on_qaChannelListen_triggered();
	void on_qaChannelAdd_triggered();
	void on_qaChannelRemove_triggered();
	void on_qaChannelACL_triggered();
	void on_qaChannelLink_triggered();
	void on_qaChannelUnlink_triggered();
	void on_qaChannelUnlinkAll_triggered();
	void on_qaChannelSendMessage_triggered();
	void on_qaChannelFilter_triggered();
	void on_qaChannelCopyURL_triggered();
	void on_qaListenerLocalVolume_triggered();
	void on_qaAudioReset_triggered();
	void on_qaAudioMute_triggered();
	void on_qaAudioDeaf_triggered();
	void on_qaRecording_triggered();
	void on_qaAudioTTS_triggered();
	void on_qaAudioUnlink_triggered();
	void on_qaAudioStats_triggered();
	void on_qaConfigDialog_triggered();
	void on_qaConfigHideFrame_triggered();
	void on_qmConfig_aboutToShow();
	void on_qaConfigMinimal_triggered();
	void on_qaConfigCert_triggered();
	void on_qaAudioWizard_triggered();
	void on_qaDeveloperConsole_triggered();
	void on_qaHelpWhatsThis_triggered();
	void on_qaHelpAbout_triggered();
	void on_qaHelpAboutQt_triggered();
	void on_qaHelpVersionCheck_triggered();
	void on_qaQuit_triggered();
	void on_qaHide_triggered();
	void on_qteChat_tabPressed();
	void on_qteChat_backtabPressed();
	void on_qteChat_ctrlSpacePressed();
	void on_qtvUsers_customContextMenuRequested(const QPoint &mpos, bool usePositionForGettingContext = true);
	void on_qteLog_customContextMenuRequested(const QPoint &pos);
	void on_qteLog_anchorClicked(const QUrl &);
	void on_qteLog_highlighted(const QUrl &link);
	void on_PushToTalk_triggered(bool, QVariant);
	void on_PushToMute_triggered(bool, QVariant);
	void on_VolumeUp_triggered(bool, QVariant);
	void on_VolumeDown_triggered(bool, QVariant);
	void on_gsMuteSelf_down(QVariant);
	void on_gsDeafSelf_down(QVariant);
	void on_gsWhisper_triggered(bool, QVariant);
	void addTarget(ShortcutTarget *);
	void removeTarget(ShortcutTarget *);
	void on_gsCycleTransmitMode_triggered(bool, QVariant);
	void on_gsToggleMainWindowVisibility_triggered(bool, QVariant);
	void on_gsTransmitModePushToTalk_triggered(bool, QVariant);
	void on_gsTransmitModeContinuous_triggered(bool, QVariant);
	void on_gsTransmitModeVAD_triggered(bool, QVariant);
	void on_gsSendTextMessage_triggered(bool, QVariant);
	void on_gsSendClipboardTextMessage_triggered(bool, QVariant);
	void on_gsToggleTalkingUI_triggered(bool, QVariant);
	void on_gsToggleSearch_triggered(bool, QVariant);
	void on_Reconnect_timeout();
	void on_Icon_activated(QSystemTrayIcon::ActivationReason);
	void on_qaTalkingUIToggle_triggered();
	void voiceRecorderDialog_finished(int);
	void qtvUserCurrentChanged(const QModelIndex &, const QModelIndex &);
	void serverConnected();
	void serverDisconnected(QAbstractSocket::SocketError, QString reason);
	void resolverError(QAbstractSocket::SocketError, QString reason);
	void viewCertificate(bool);
	void openUrl(const QUrl &url);
	void context_triggered();
	void updateTarget();
	void updateMenuPermissions();
	/// Handles state changes like talking mode changes and mute/unmute
	/// or priority speaker flag changes for the gui user
	void userStateChanged();
	void destroyUserInformation();
	void trayAboutToShow();
	void sendChatbarMessage(QString msg);
	void sendChatbarText(QString msg);
	void pttReleased();
	void whisperReleased(QVariant scdata);
	void onResetAudio();
	void showRaiseWindow();
	void on_qaFilterToggle_triggered();
	/// Opens a save dialog for the image referenced by qtcSaveImageCursor.
	void saveImageAs();
	/// Returns the path to the user's image directory, optionally with a
	/// filename included.
	QString getImagePath(QString filename = QString()) const;
	/// Updates the user's image directory to the given path (any included
	/// filename is discarded).
	void updateImagePath(QString filepath) const;
	void setTransmissionMode(Settings::AudioTransmit mode);
	/// Sets the local user's mute state
	///
	/// @param mute Whether to mute the user
	void setAudioMute(bool mute);
	/// Sets the local user's deaf state
	///
	/// @param deaf Whether to deafen the user
	void setAudioDeaf(bool deaf);
	// Callback the search action being triggered
	void on_qaSearch_triggered();
	void toggleSearchDialogVisibility();
signals:
	/// Signal emitted when the server and the client have finished
	/// synchronizing (after a new connection).
	void serverSynchronized();
	/// Signal emitted whenever a user adds a new ChannelListener
	void userAddedChannelListener(ClientUser *user, Channel *channel);
	/// Signal emitted whenever a user removes a ChannelListener
	void userRemovedChannelListener(ClientUser *user, Channel *channel);
	void transmissionModeChanged(Settings::AudioTransmit newMode);

public:
	MainWindow(QWidget *parent);
	~MainWindow() Q_DECL_OVERRIDE;

	// From msgHandler. Implementation in Messages.cpp
#define MUMBLE_MH_MSG(x) void msg##x(const MumbleProto::x &);
	MUMBLE_MH_ALL
#undef MUMBLE_MH_MSG
	void removeContextAction(const MumbleProto::ContextActionModify &msg);
	/// Logs a message that an action could not be saved permanently because
	/// the user has no certificate and can't be reliably identified.
	///
	/// @param actionName  The name of the action that has been executed.
	/// @param p  The user on which the action was performed.
	void logChangeNotPermanent(const QString &actionName, ClientUser *const p) const;
};

#endif
