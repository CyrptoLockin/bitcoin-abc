// Copyright (c) 2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/node.h>

#include <chain.h>
#include <chainparams.h>
#include <config.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/wallet.h>
#include <net.h>
#include <netaddress.h>
#include <netbase.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <sync.h>
#include <txmempool.h>
#include <ui_interface.h>
#include <util.h>
#include <validation.h>
#include <warnings.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif
#ifdef ENABLE_WALLET
#define CHECK_WALLET(x) x
#else
#define CHECK_WALLET(x)                                                        \
    throw std::logic_error("Wallet function called in non-wallet build.")
#endif

#include <boost/thread/thread.hpp>

#include <atomic>

class CWallet;
class HTTPRPCRequestProcessor;

namespace interfaces {
namespace {

    class NodeImpl : public Node {
        void parseParameters(int argc, const char *const argv[]) override {
            gArgs.ParseParameters(argc, argv);
        }
        void readConfigFile(const std::string &conf_path) override {
            gArgs.ReadConfigFile(conf_path);
        }
        bool softSetArg(const std::string &arg,
                        const std::string &value) override {
            return gArgs.SoftSetArg(arg, value);
        }
        bool softSetBoolArg(const std::string &arg, bool value) override {
            return gArgs.SoftSetBoolArg(arg, value);
        }
        void selectParams(const std::string &network) override {
            SelectParams(network);
        }
        void initLogging() override { InitLogging(); }
        void initParameterInteraction() override { InitParameterInteraction(); }
        std::string getWarnings(const std::string &type) override {
            return GetWarnings(type);
        }
        bool baseInitialize(Config &config, RPCServer &rpcServer) override {
            return AppInitBasicSetup() &&
                   AppInitParameterInteraction(config, rpcServer) &&
                   AppInitSanityChecks() && AppInitLockDataDirectory();
        }
        bool
        appInitMain(Config &config,
                    HTTPRPCRequestProcessor &httpRPCRequestProcessor) override {
            return AppInitMain(config, httpRPCRequestProcessor);
        }
        void appShutdown() override {
            Interrupt();
            Shutdown();
        }
        void startShutdown() override { StartShutdown(); }
        bool shutdownRequested() override { return ShutdownRequested(); }
        void mapPort(bool use_upnp) override {
            if (use_upnp) {
                StartMapPort();
            } else {
                InterruptMapPort();
                StopMapPort();
            }
        }
        std::string helpMessage(HelpMessageMode mode) override {
            return HelpMessage(mode);
        }
        bool getProxy(Network net, proxyType &proxy_info) override {
            return GetProxy(net, proxy_info);
        }
        size_t getNodeCount(CConnman::NumConnections flags) override {
            return g_connman ? g_connman->GetNodeCount(flags) : 0;
        }
        int64_t getTotalBytesRecv() override {
            return g_connman ? g_connman->GetTotalBytesRecv() : 0;
        }
        int64_t getTotalBytesSent() override {
            return g_connman ? g_connman->GetTotalBytesSent() : 0;
        }
        size_t getMempoolSize() override { return g_mempool.size(); }
        size_t getMempoolDynamicUsage() override {
            return g_mempool.DynamicMemoryUsage();
        }
        bool getHeaderTip(int &height, int64_t &block_time) override {
            LOCK(::cs_main);
            if (::pindexBestHeader) {
                height = ::pindexBestHeader->nHeight;
                block_time = ::pindexBestHeader->GetBlockTime();
                return true;
            }
            return false;
        }
        int getNumBlocks() override {
            LOCK(::cs_main);
            return ::chainActive.Height();
        }
        int64_t getLastBlockTime() override {
            LOCK(::cs_main);
            if (::chainActive.Tip()) {
                return ::chainActive.Tip()->GetBlockTime();
            }
            // Genesis block's time of current network
            return Params().GenesisBlock().GetBlockTime();
        }
        double getVerificationProgress() override {
            const CBlockIndex *tip;
            {
                LOCK(::cs_main);
                tip = ::chainActive.Tip();
            }
            return GuessVerificationProgress(::Params().TxData(), tip);
        }
        bool isInitialBlockDownload() override {
            return IsInitialBlockDownload();
        }
        bool getReindex() override { return ::fReindex; }
        bool getImporting() override { return ::fImporting; }
        void setNetworkActive(bool active) override {
            if (g_connman) {
                g_connman->SetNetworkActive(active);
            }
        }
        bool getNetworkActive() override {
            return g_connman && g_connman->GetNetworkActive();
        }
        std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) override {
            return MakeHandler(::uiInterface.InitMessage.connect(fn));
        }
        std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) override {
            return MakeHandler(::uiInterface.ThreadSafeMessageBox.connect(fn));
        }
        std::unique_ptr<Handler> handleQuestion(QuestionFn fn) override {
            return MakeHandler(::uiInterface.ThreadSafeQuestion.connect(fn));
        }
        std::unique_ptr<Handler>
        handleShowProgress(ShowProgressFn fn) override {
            return MakeHandler(::uiInterface.ShowProgress.connect(fn));
        }
        std::unique_ptr<Handler> handleLoadWallet(LoadWalletFn fn) override {
            CHECK_WALLET(return MakeHandler(::uiInterface.LoadWallet.connect(
                [fn](CWallet *wallet) { fn(MakeWallet(*wallet)); })));
        }
        std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(
            NotifyNumConnectionsChangedFn fn) override {
            return MakeHandler(
                ::uiInterface.NotifyNumConnectionsChanged.connect(fn));
        }
        std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(
            NotifyNetworkActiveChangedFn fn) override {
            return MakeHandler(
                ::uiInterface.NotifyNetworkActiveChanged.connect(fn));
        }
        std::unique_ptr<Handler>
        handleNotifyAlertChanged(NotifyAlertChangedFn fn) override {
            return MakeHandler(::uiInterface.NotifyAlertChanged.connect(fn));
        }
        std::unique_ptr<Handler>
        handleBannedListChanged(BannedListChangedFn fn) override {
            return MakeHandler(::uiInterface.BannedListChanged.connect(fn));
        }
        std::unique_ptr<Handler>
        handleNotifyBlockTip(NotifyBlockTipFn fn) override {
            return MakeHandler(::uiInterface.NotifyBlockTip.connect(
                [fn](bool initial_download, const CBlockIndex *block) {
                    fn(initial_download, block->nHeight, block->GetBlockTime(),
                       GuessVerificationProgress(::Params().TxData(), block));
                }));
        }
        std::unique_ptr<Handler>
        handleNotifyHeaderTip(NotifyHeaderTipFn fn) override {
            return MakeHandler(::uiInterface.NotifyHeaderTip.connect(
                [fn](bool initial_download, const CBlockIndex *block) {
                    fn(initial_download, block->nHeight, block->GetBlockTime(),
                       GuessVerificationProgress(::Params().TxData(), block));
                }));
        }
    };

} // namespace

std::unique_ptr<Node> MakeNode() {
    return std::make_unique<NodeImpl>();
}

} // namespace interfaces
