#include "clientmodel.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "optionsmodel.h"
#include "transactiontablemodel.h"

#include "alert.h"
#include "chainparams.h"
#include "main.h"
#include "ui_interface.h"

#include <QDateTime>
#include <QDebug>
#include <QTimer>

#include <boost/bind/bind.hpp>
using namespace boost::placeholders;

static const int64_t nClientStartupTime = GetTime();

ClientModel::ClientModel(OptionsModel* optionsModel, QObject* parent)
    : QObject(parent),
      optionsModel(optionsModel),
      cachedNumBlocks(0),
      numBlocksAtStartup(-1),
      pollTimer(0) {
	qRegisterMetaType<CNodeShortStats>("CNodeShortStats");

	pollTimer = new QTimer(this);
	pollTimer->setInterval(MODEL_UPDATE_DELAY);
	pollTimer->start();
	connect(pollTimer, SIGNAL(timeout()), this, SLOT(updateTimer()));

	subscribeToCoreSignals();
}

ClientModel::~ClientModel() {
	unsubscribeFromCoreSignals();
}

CNodeShortStats ClientModel::getConnections() const {
	CNodeShortStats vStats;
	{
		LOCK(cs_vNodes);
		for (const CNode* pNode : vNodes) {
			CNodeShortStat nodeStat = {pNode->addrName, pNode->nVersion, pNode->strSubVer,
			                           pNode->nStartingHeight};
			vStats.push_back(nodeStat);
		}
	}
	return vStats;
}

int ClientModel::getNumConnections() const {
	return vNodes.size();
}

int ClientModel::getNumBlocks() const {
	LOCK(cs_main);
	return nBestHeight;
}

int ClientModel::getPegSupplyIndex() const {
	LOCK(cs_main);
	CBlockIndex* pblockindex = pindexBest;
	return pblockindex->nPegSupplyIndex;
}

int ClientModel::getPegNextSupplyIndex() const {
	LOCK(cs_main);
	CBlockIndex* pblockindex = pindexBest;
	return pblockindex->GetNextIntervalPegSupplyIndex();
}

int ClientModel::getPegNextNextSupplyIndex() const {
	LOCK(cs_main);
	CBlockIndex* pblockindex = pindexBest;
	return pblockindex->GetNextNextIntervalPegSupplyIndex();
}

int ClientModel::getPegStartBlockNum() const {
	LOCK(cs_main);
	return nPegStartHeight;
}

boost::tuple<int, int, int> ClientModel::getPegVotes() const {
	LOCK(cs_main);
	CBlockIndex* pblockindex = pindexBest;
	return boost::make_tuple(pblockindex->nPegVotesInflate, pblockindex->nPegVotesDeflate,
	                         pblockindex->nPegVotesNochange);
}

int ClientModel::getNumBlocksAtStartup() {
	if (numBlocksAtStartup == -1)
		numBlocksAtStartup = getNumBlocks();
	return numBlocksAtStartup;
}

quint64 ClientModel::getTotalBytesRecv() const {
	return CNode::GetTotalBytesRecv();
}

quint64 ClientModel::getTotalBytesSent() const {
	return CNode::GetTotalBytesSent();
}

QDateTime ClientModel::getLastBlockDate() const {
	LOCK(cs_main);
	if (pindexBest)
		return QDateTime::fromTime_t(pindexBest->GetBlockTime());
	else
		return QDateTime::fromTime_t(
		    Params().GenesisBlock().nTime);  // Genesis block's time of current network
}

void ClientModel::updateTimer() {
	// Get required lock upfront. This avoids the GUI from getting stuck on
	// periodical polls if the core is holding the locks for a longer time -
	// for example, during a wallet rescan.
	TRY_LOCK(cs_main, lockMain);
	if (!lockMain)
		return;
	// Some quantities (such as number of blocks) change so fast that we don't want to be notified
	// for each change. Periodically check and update with a timer.
	int newNumBlocks = getNumBlocks();

	if (cachedNumBlocks != newNumBlocks) {
		cachedNumBlocks = newNumBlocks;

		emit numBlocksChanged(newNumBlocks);
	}

	emit bytesChanged(getTotalBytesRecv(), getTotalBytesSent());
}

void ClientModel::updateNumConnections(int numConnections) {
	emit numConnectionsChanged(numConnections);
}

void ClientModel::updateConnections(const CNodeShortStats& stats) {
	emit connectionsChanged(stats);
}

void ClientModel::updateAlert(const QString& hash, int status) {
	// Show error message notification for new alert
	if (status == CT_NEW) {
		uint256 hash_256;
		hash_256.SetHex(hash.toStdString());
		CAlert alert = CAlert::getAlertByHash(hash_256);
		if (!alert.IsNull()) {
			emit message(tr("Network Alert"), QString::fromStdString(alert.strStatusBar), false,
			             CClientUIInterface::ICON_ERROR);
		}
	}

	emit alertsChanged(getStatusBarWarnings());
}

bool ClientModel::isTestNet() const {
	return TestNet();
}

bool ClientModel::inInitialBlockDownload() const {
	return IsInitialBlockDownload();
}

bool ClientModel::isImporting() const {
	return fImporting;
}

QString ClientModel::getStatusBarWarnings() const {
	return QString::fromStdString(GetWarnings("statusbar"));
}

OptionsModel* ClientModel::getOptionsModel() {
	return optionsModel;
}

QString ClientModel::formatFullVersion() const {
	return QString::fromStdString(FormatFullVersion());
}

QString ClientModel::formatBuildDate() const {
	return QString::fromStdString(CLIENT_DATE);
}

bool ClientModel::isReleaseVersion() const {
	return CLIENT_VERSION_IS_RELEASE;
}

QString ClientModel::clientName() const {
	return QString::fromStdString(CLIENT_NAME);
}

QString ClientModel::formatClientStartupTime() const {
	return QDateTime::fromTime_t(nClientStartupTime).toString();
}

static void NotifyNumConnectionsChanged(ClientModel* clientmodel, int newNumConnections) {
	// Too noisy: qDebug() << "NotifyNumConnectionsChanged : " + QString::number(newNumConnections);
	QMetaObject::invokeMethod(clientmodel, "updateNumConnections", Qt::QueuedConnection,
	                          Q_ARG(int, newNumConnections));
}

static void NotifyConnectionsChanged(ClientModel* clientmodel, const CNodeShortStats& stats) {
	// Too noisy: qDebug() << "NotifyConnectionsChanged : " + QString::number(stats.size());
	CNodeShortStats stats_copy = stats;
	QMetaObject::invokeMethod(clientmodel, "updateConnections", Qt::QueuedConnection,
	                          Q_ARG(CNodeShortStats, stats_copy));
}

static void NotifyAlertChanged(ClientModel* clientmodel, const uint256& hash, ChangeType status) {
	qDebug() << "NotifyAlertChanged : " + QString::fromStdString(hash.GetHex()) +
	                " status=" + QString::number(status);
	QMetaObject::invokeMethod(clientmodel, "updateAlert", Qt::QueuedConnection,
	                          Q_ARG(QString, QString::fromStdString(hash.GetHex())),
	                          Q_ARG(int, status));
}

void ClientModel::subscribeToCoreSignals() {
	// Connect signals to client
	uiInterface.NotifyNumConnectionsChanged.connect(
	    boost::bind(NotifyNumConnectionsChanged, this, _1));
	uiInterface.NotifyConnectionsChanged.connect(boost::bind(NotifyConnectionsChanged, this, _1));
	uiInterface.NotifyAlertChanged.connect(boost::bind(NotifyAlertChanged, this, _1, _2));
}

void ClientModel::unsubscribeFromCoreSignals() {
	// Disconnect signals from client
	uiInterface.NotifyNumConnectionsChanged.disconnect(
	    boost::bind(NotifyNumConnectionsChanged, this, _1));
	uiInterface.NotifyConnectionsChanged.disconnect(
	    boost::bind(NotifyConnectionsChanged, this, _1));
	uiInterface.NotifyAlertChanged.disconnect(boost::bind(NotifyAlertChanged, this, _1, _2));
}
