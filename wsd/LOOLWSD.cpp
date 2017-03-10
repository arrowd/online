/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include "LOOLWSD.hpp"

/* Default host used in the start test URI */
#define LOOLWSD_TEST_HOST "localhost"

/* Default loleaflet UI used in the admin console URI */
#define LOOLWSD_TEST_ADMIN_CONSOLE "/loleaflet/dist/admin/admin.html"

/* Default loleaflet UI used in the start test URI */
#define LOOLWSD_TEST_LOLEAFLET_UI "/loleaflet/" LOOLWSD_VERSION_HASH "/loleaflet.html"

/* Default document used in the start test URI */
#define LOOLWSD_TEST_DOCUMENT_RELATIVE_PATH "test/data/hello-world.odt"

// This is the main source for the loolwsd program. LOOL uses several loolwsd processes: one main
// parent process that listens on the TCP port and accepts connections from LOOL clients, and a
// number of child processes, each which handles a viewing (editing) session for one document.

#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cassert>
#include <cerrno>
#include <clocale>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <Poco/DateTimeFormatter.h>
#include <Poco/DOM/AutoPtr.h>
#include <Poco/DOM/DOMParser.h>
#include <Poco/DOM/DOMWriter.h>
#include <Poco/DOM/Document.h>
#include <Poco/DOM/Element.h>
#include <Poco/DOM/NodeList.h>
#include <Poco/Environment.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/MemoryStream.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/ConsoleCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/KeyConsoleHandler.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/NameValueCollection.h>
#include <Poco/Net/Net.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/PartHandler.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Path.h>
#include <Poco/Pipe.h>
#include <Poco/Process.h>
#include <Poco/SAX/InputSource.h>
#include <Poco/StreamCopier.h>
#include <Poco/StringTokenizer.h>
#include <Poco/TemporaryFile.h>
#include <Poco/ThreadPool.h>
#include <Poco/URI.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/MapConfiguration.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

#include "Admin.hpp"
#include "Auth.hpp"
#include "ClientSession.hpp"
#include "Common.hpp"
#include "DocumentBroker.hpp"
#include "Exceptions.hpp"
#include "FileServer.hpp"
#include "IoUtil.hpp"
#include "Log.hpp"
#include "Protocol.hpp"
#include "ServerSocket.hpp"
#include "Session.hpp"
#if ENABLE_SSL
#include "SslSocket.hpp"
#endif
#include "Storage.hpp"
#include "TraceFile.hpp"
#include "Unit.hpp"
#include "UnitHTTP.hpp"
#include "UserMessages.hpp"
#include "Util.hpp"
#include "FileUtil.hpp"

#ifdef KIT_IN_PROCESS
#include <Kit.hpp>
#endif

#ifdef FUZZER
#include <tools/Replay.hpp>
#endif

#include "common/SigUtil.hpp"

using namespace LOOLProtocol;

using Poco::Environment;
using Poco::Exception;
using Poco::File;
using Poco::Net::HTMLForm;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::MessageHeader;
using Poco::Net::NameValueCollection;
using Poco::Net::PartHandler;
using Poco::Net::SecureServerSocket;
using Poco::Net::SocketAddress;
using Poco::Net::WebSocket;
using Poco::Path;
using Poco::Pipe;
using Poco::Process;
using Poco::ProcessHandle;
using Poco::StreamCopier;
using Poco::StringTokenizer;
using Poco::TemporaryFile;
#if FUZZER
using Poco::Thread;
#endif
using Poco::ThreadPool;
using Poco::URI;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;
using Poco::XML::AutoPtr;
using Poco::XML::DOMParser;
using Poco::XML::DOMWriter;
using Poco::XML::Element;
using Poco::XML::InputSource;
using Poco::XML::NodeList;
using Poco::XML::Node;

int ClientPortNumber = DEFAULT_CLIENT_PORT_NUMBER;
int MasterPortNumber = DEFAULT_MASTER_PORT_NUMBER;

/// New LOK child processes ready to host documents.
//TODO: Move to a more sensible namespace.
static bool DisplayVersion = false;

// Tracks the set of prisoners / children waiting to be used.
static std::mutex NewChildrenMutex;
static std::condition_variable NewChildrenCV;
static std::vector<std::shared_ptr<ChildProcess> > NewChildren;

static std::chrono::steady_clock::time_point LastForkRequestTime = std::chrono::steady_clock::now();
static std::atomic<int> OutstandingForks(0);
static std::map<std::string, std::shared_ptr<DocumentBroker> > DocBrokers;
static std::mutex DocBrokersMutex;

#if 0 // loolnb
/// Used when shutting down to notify them all that the server is recycling.
static std::vector<std::shared_ptr<LOOLWebSocket> > ClientWebSockets;
static std::mutex ClientWebSocketsMutex;
#endif

extern "C" { void dump_state(void); /* easy for gdb */ }

#if ENABLE_DEBUG
static int careerSpanSeconds = 0;
#endif

namespace
{

inline void shutdownLimitReached(WebSocketHandler& ws)
{
    const std::string error = Poco::format(PAYLOAD_UNAVAILABLE_LIMIT_REACHED, MAX_DOCUMENTS, MAX_CONNECTIONS);
    LOG_INF("Sending client limit-reached message: " << error);

    try
    {
        // Let the client know we are shutting down.
        ws.sendFrame(error);

        // Shutdown.
        ws.shutdown(WebSocketHandler::StatusCodes::POLICY_VIOLATION);
    }
    catch (const std::exception& ex)
    {
        LOG_ERR("Error while shuting down socket on reaching limit: " << ex.what());
    }
}

/// Internal implementation to alert all clients
/// connected to any document.
void alertAllUsersInternal(const std::string& msg)
{
    Util::assertIsLocked(DocBrokersMutex);

    LOG_INF("Alerting all users: [" << msg << "]");

    for (auto& brokerIt : DocBrokers)
    {
        auto lock = brokerIt.second->getLock();
        brokerIt.second->alertAllUsers(msg);
    }
}
}

/// Remove dead and idle DocBrokers.
/// The client of idle document should've greyed-out long ago.
/// Returns true if at least one is removed.
bool cleanupDocBrokers()
{
    Util::assertIsLocked(DocBrokersMutex);

    const auto count = DocBrokers.size();
    for (auto it = DocBrokers.begin(); it != DocBrokers.end(); )
    {
        auto docBroker = it->second;
        auto lock = docBroker->getDeferredLock();
        if (!lock.try_lock())
        {
            // Document busy at the moment, cleanup later.
            ++it;
            continue;
        }

        // Remove idle documents after 1 hour.
        const bool idle = (docBroker->getIdleTimeSecs() >= 3600);

        // Cleanup used and dead entries.
        if (docBroker->isLoaded() &&
            (docBroker->getSessionsCount() == 0 || !docBroker->isAlive() || idle))
        {
            LOG_INF("Removing " << (idle ? "idle" : "dead") <<
                    " DocumentBroker for docKey [" << it->first << "].");
            it = DocBrokers.erase(it);
            docBroker->terminateChild(lock, idle ? "idle" : "");
        }
        else
        {
            ++it;
        }
    }

    if (count != DocBrokers.size())
    {
        LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after cleanup.");
        for (auto& pair : DocBrokers)
        {
            LOG_TRC("DocumentBroker [" << pair.first << "].");
        }

        return true;
    }

    return false;
}

/// Forks as many children as requested.
/// Returns the number of children requested to spawn,
/// -1 for error.
static bool forkChildren(const int number)
{
    Util::assertIsLocked(DocBrokersMutex);
    Util::assertIsLocked(NewChildrenMutex);

    if (number > 0)
    {
        const std::string fs = FileUtil::checkDiskSpaceOnRegisteredFileSystems(false);
        if (!fs.empty())
        {
            LOG_WRN("File system of " << fs << " dangerously low on disk space");
            alertAllUsersInternal("error: cmd=internal kind=diskfull");
        }

#ifdef KIT_IN_PROCESS
        forkLibreOfficeKit(LOOLWSD::ChildRoot, LOOLWSD::SysTemplate, LOOLWSD::LoTemplate, LO_JAIL_SUBPATH, number);
#else
        const std::string aMessage = "spawn " + std::to_string(number) + "\n";
        LOG_DBG("MasterToForKit: " << aMessage.substr(0, aMessage.length() - 1));
        if (IoUtil::writeToPipe(LOOLWSD::ForKitWritePipe, aMessage) > 0)
#endif
        {
            OutstandingForks += number;
            LastForkRequestTime = std::chrono::steady_clock::now();
            return number;
        }

        LOG_ERR("No forkit pipe while rebalancing children.");
        return -1; // Fail.
    }

    return 0;
}

/// Cleans up dead children.
/// Returns true if removed at least one.
static bool cleanupChildren()
{
    Util::assertIsLocked(NewChildrenMutex);

    bool removed = false;
    for (int i = NewChildren.size() - 1; i >= 0; --i)
    {
        if (!NewChildren[i]->isAlive())
        {
            LOG_WRN("Removing dead spare child [" << NewChildren[i]->getPid() << "].");

            NewChildren.erase(NewChildren.begin() + i);
            removed = true;
        }
    }

    return removed;
}

/// Decides how many children need spawning and spanws.
/// When force is true, no check of elapsed time since last request is done.
/// Returns the number of children requested to spawn,
/// -1 for error.
static int rebalanceChildren(int balance)
{
    Util::assertIsLocked(DocBrokersMutex);
    Util::assertIsLocked(NewChildrenMutex);

    // Do the cleanup first.
    const bool rebalance = cleanupChildren();

    const auto duration = (std::chrono::steady_clock::now() - LastForkRequestTime);
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    if (OutstandingForks > 0 && durationMs >= CHILD_TIMEOUT_MS)
    {
        // Children taking too long to spawn.
        // Forget we had requested any, and request anew.
        LOG_WRN("ForKit not responsive for " << durationMs << " ms forking " <<
                OutstandingForks << " children. Resetting.");
        OutstandingForks = 0;
    }

    const auto available = NewChildren.size();
    balance -= available;
    balance -= OutstandingForks;

    if (balance > 0 && (rebalance || OutstandingForks == 0))
    {
        LOG_DBG("prespawnChildren: Have " << available << " spare " <<
                (available == 1 ? "child" : "children") << ", and " <<
                OutstandingForks << " outstanding, forking " << balance << " more.");
        return forkChildren(balance);
    }

    return 0;
}

/// Proactively spawn children processes
/// to load documents with alacrity.
/// Returns true only if at least one child was requested to spawn.
static bool prespawnChildren()
{
#if 1 // FIXME: why re-balance DockBrokers here ? ...
    // First remove dead DocBrokers, if possible.
    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex, std::defer_lock);
    if (!docBrokersLock.try_lock())
    {
        // Busy, try again later.
        return false;
    }

    cleanupDocBrokers();
#endif

    std::unique_lock<std::mutex> lock(NewChildrenMutex, std::defer_lock);
    if (!lock.try_lock())
    {
        // We are forking already? Try later.
        return false;
    }

    const int numPreSpawn = LOOLWSD::NumPreSpawnedChildren;
    return (rebalanceChildren(numPreSpawn) > 0);
}

static size_t addNewChild(const std::shared_ptr<ChildProcess>& child)
{
    std::unique_lock<std::mutex> lock(NewChildrenMutex);

    --OutstandingForks;
    NewChildren.emplace_back(child);
    const auto count = NewChildren.size();
    LOG_INF("Have " << count << " spare " <<
            (count == 1 ? "child" : "children") << " after adding [" << child->getPid() << "].");
    lock.unlock();

    NewChildrenCV.notify_one();
    return count;
}

std::shared_ptr<ChildProcess> getNewChild_Blocks()
{
    std::unique_lock<std::mutex> locka(DocBrokersMutex);
    std::unique_lock<std::mutex> lockb(NewChildrenMutex);

    namespace chrono = std::chrono;
    const auto startTime = chrono::steady_clock::now();
    do
    {
        LOG_DBG("getNewChild: Rebalancing children.");
        int numPreSpawn = LOOLWSD::NumPreSpawnedChildren;
        ++numPreSpawn; // Replace the one we'll dispatch just now.
        if (rebalanceChildren(numPreSpawn) < 0)
        {
            // Fatal. Let's fail and retry at a higher level.
            LOG_DBG("getNewChild: rebalancing of children failed.");
            return nullptr;
        }

        // With valgrind we need extended time to spawn kits.
#ifdef KIT_IN_PROCESS
        const auto timeoutMs = CHILD_TIMEOUT_MS;
#else
        const auto timeoutMs = CHILD_TIMEOUT_MS * (LOOLWSD::NoCapsForKit ? 100 : 1);
#endif
        LOG_TRC("Waiting for a new child for a max of " << timeoutMs << " ms.");
        const auto timeout = chrono::milliseconds(timeoutMs);
        // FIXME: blocks ...
        if (NewChildrenCV.wait_for(lockb, timeout, []() { return !NewChildren.empty(); }))
        {
            auto child = NewChildren.back();
            NewChildren.pop_back();
            const auto available = NewChildren.size();

            // Validate before returning.
            if (child && child->isAlive())
            {
                LOG_DBG("getNewChild: Have " << available << " spare " <<
                        (available == 1 ? "child" : "children") <<
                        " after poping [" << child->getPid() << "] to return.");
                return child;
            }

            LOG_WRN("getNewChild: popped dead child, need to find another.");
        }
        else
        {
            LOG_WRN("getNewChild: No available child. Sending spawn request to forkit and failing.");
        }
    }
    while (chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - startTime).count() <
           CHILD_TIMEOUT_MS * 4);

    LOG_DBG("getNewChild: Timed out while waiting for new child.");
    return nullptr;
}

/// Handles the filename part of the convert-to POST request payload.
class ConvertToPartHandler : public PartHandler
{
    std::string& _filename;
public:
    ConvertToPartHandler(std::string& filename)
        : _filename(filename)
    {
    }

    virtual void handlePart(const MessageHeader& header, std::istream& stream) override
    {
        // Extract filename and put it to a temporary directory.
        std::string disp;
        NameValueCollection params;
        if (header.has("Content-Disposition"))
        {
            std::string cd = header.get("Content-Disposition");
            MessageHeader::splitParameters(cd, disp, params);
        }

        if (!params.has("filename"))
            return;

        Path tempPath = Path::forDirectory(Poco::TemporaryFile::tempName() + "/");
        File(tempPath).createDirectories();
        // Prevent user inputting anything funny here.
        // A "filename" should always be a filename, not a path
        const Path filenameParam(params.get("filename"));
        tempPath.setFileName(filenameParam.getFileName());
        _filename = tempPath.toString();

        // Copy the stream to _filename.
        std::ofstream fileStream;
        fileStream.open(_filename);
        StreamCopier::copyStream(stream, fileStream);
        fileStream.close();
    }
};

namespace
{

inline std::string getLaunchBase(const std::string &credentials)
{
    std::ostringstream oss;
    oss << "    ";
    oss << ((LOOLWSD::isSSLEnabled() || LOOLWSD::isSSLTermination()) ? "https://" : "http://");
    oss << credentials;
    oss << LOOLWSD_TEST_HOST ":";
    oss << ClientPortNumber;

    return oss.str();
}

inline std::string getLaunchURI()
{
    const std::string aAbsTopSrcDir = Poco::Path(Application::instance().commandPath()).parent().toString();

    std::ostringstream oss;

    oss << getLaunchBase("");
    oss << LOOLWSD_TEST_LOLEAFLET_UI;
    oss << "?file_path=file://";
    oss << Poco::Path(aAbsTopSrcDir).absolute().toString();
    oss << LOOLWSD_TEST_DOCUMENT_RELATIVE_PATH;

    return oss.str();
}

inline std::string getAdminURI(const Poco::Util::LayeredConfiguration &config)
{
    std::string user = config.getString("admin_console.username", "");
    std::string passwd = config.getString("admin_console.password", "");

    if (user.empty() || passwd.empty())
        return "";

    std::ostringstream oss;

    oss << getLaunchBase(user + ":" + passwd + "@");
    oss << LOOLWSD_TEST_ADMIN_CONSOLE;

    return oss.str();
}

} // anonymous namespace

std::atomic<unsigned> LOOLWSD::NextSessionId;
#ifndef KIT_IN_PROCESS
std::atomic<int> LOOLWSD::ForKitWritePipe(-1);
std::atomic<int> LOOLWSD::ForKitProcId(-1);
bool LOOLWSD::NoCapsForKit = false;
#endif
#ifdef FUZZER
bool LOOLWSD::DummyLOK = false;
std::string LOOLWSD::FuzzFileName = "";
#endif
std::string LOOLWSD::Cache = LOOLWSD_CACHEDIR;
std::string LOOLWSD::SysTemplate;
std::string LOOLWSD::LoTemplate;
std::string LOOLWSD::ChildRoot;
std::string LOOLWSD::ServerName;
std::string LOOLWSD::FileServerRoot;
std::string LOOLWSD::LOKitVersion;
std::string LOOLWSD::ConfigFile = LOOLWSD_CONFIGDIR "/loolwsd.xml";
Util::RuntimeConstant<bool> LOOLWSD::SSLEnabled;
Util::RuntimeConstant<bool> LOOLWSD::SSLTermination;

static std::string UnitTestLibrary;

unsigned int LOOLWSD::NumPreSpawnedChildren = 0;
std::atomic<unsigned> LOOLWSD::NumConnections;
std::unique_ptr<TraceFileWriter> LOOLWSD::TraceDumper;

/// This thread polls basic web serving, and handling of
/// websockets before upgrade: when upgraded they go to the
/// relevant DocumentBroker poll instead.
TerminatingPoll WebServerPoll("websrv_poll");

class PrisonerPoll : public TerminatingPoll {
public:
    PrisonerPoll() : TerminatingPoll("prisoner_poll") {}

    /// Check prisoners are still alive and balaned.
    void wakeupHook() override;
};

/// This thread listens for and accepts prisoner kit processes.
/// And also cleans up and balances the correct number of childen.
PrisonerPoll PrisonerPoll;

/// Helper class to hold default configuration entries.
class AppConfigMap final : public Poco::Util::MapConfiguration
{
public:
    AppConfigMap(const std::map<std::string, std::string>& map)
    {
        for (const auto& pair : map)
        {
            setRaw(pair.first, pair.second);
        }
    }
};

LOOLWSD::LOOLWSD()
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::initialize(Application& self)
{
    if (geteuid() == 0)
    {
        throw std::runtime_error("Do not run as root. Please run as lool user.");
    }

    if (!UnitWSD::init(UnitWSD::UnitType::Wsd, UnitTestLibrary))
    {
        throw std::runtime_error("Failed to load wsd unit test library.");
    }

    auto& conf = config();

    // Add default values of new entries here.
    static const std::map<std::string, std::string> DefAppConfig
        = { { "tile_cache_path", LOOLWSD_CACHEDIR },
            { "sys_template_path", "systemplate" },
            { "lo_template_path", "/opt/collaboraoffice5.1" },
            { "child_root_path", "jails" },
            { "lo_jail_subpath", "lo" },
            { "server_name", "" },
            { "file_server_root_path", "loleaflet/.." },
            { "num_prespawn_children", "1" },
            { "per_document.max_concurrency", "4" },
            { "loleaflet_html", "loleaflet.html" },
            { "logging.color", "true" },
            { "logging.level", "trace" },
            { "loleaflet_logging", "false" },
            { "ssl.enable", "true" },
            { "ssl.termination", "true" },
            { "ssl.cert_file_path", LOOLWSD_CONFIGDIR "/cert.pem" },
            { "ssl.key_file_path", LOOLWSD_CONFIGDIR "/key.pem" },
            { "ssl.ca_file_path", LOOLWSD_CONFIGDIR "/ca-chain.cert.pem" },
            { "storage.filesystem[@allow]", "false" },
            { "storage.wopi[@allow]", "true" },
            { "storage.wopi.host[0][@allow]", "true" },
            { "storage.wopi.host[0]", "localhost" },
            { "storage.wopi.max_file_size", "0" },
            { "storage.webdav[@allow]", "false" },
            { "logging.file[@enable]", "false" },
            { "logging.file.property[0][@name]", "path" },
            { "logging.file.property[0]", "loolwsd.log" },
            { "logging.file.property[1][@name]", "rotation" },
            { "logging.file.property[1]", "never" },
            { "logging.file.property[2][@name]", "compress" },
            { "logging.file.property[2]", "true" },
            { "logging.file.property[3][@name]", "flush" },
            { "logging.file.property[3]", "false" },
            { "trace[@enable]", "false" } };

    // Set default values, in case they are missing from the config file.
    AutoPtr<AppConfigMap> defConfig(new AppConfigMap(DefAppConfig));
    conf.addWriteable(defConfig, PRIO_SYSTEM); // Lowest priority

    // Load default configuration files, if present.
    if (loadConfiguration(PRIO_DEFAULT) == 0)
    {
        // Fallback to the LOOLWSD_CONFIGDIR or --config-file path.
        loadConfiguration(ConfigFile, PRIO_DEFAULT);
    }

    // Override any settings passed on the command-line.
    AutoPtr<AppConfigMap> overrideConfig(new AppConfigMap(_overrideSettings));
    conf.addWriteable(overrideConfig, PRIO_APPLICATION); // Highest priority

    // Allow UT to manipulate before using configuration values.
    UnitWSD::get().configure(config());

    const auto logLevel = getConfigValue<std::string>(conf, "logging.level", "trace");
    setenv("LOOL_LOGLEVEL", logLevel.c_str(), true);
    const auto withColor = getConfigValue<bool>(conf, "logging.color", true) && isatty(fileno(stderr));
    if (withColor)
    {
        setenv("LOOL_LOGCOLOR", "1", true);
    }

    const auto logToFile = getConfigValue<bool>(conf, "logging.file[@enable]", false);
    std::map<std::string, std::string> logProperties;
    for (size_t i = 0; ; ++i)
    {
        const std::string confPath = "logging.file.property[" + std::to_string(i) + "]";
        const auto confName = config().getString(confPath + "[@name]", "");
        if (!confName.empty())
        {
            const auto value = config().getString(confPath, "");
            logProperties.emplace(confName, value);
        }
        else if (!config().has(confPath))
        {
            break;
        }
    }

    // Setup the logfile envar for the kit processes.
    if (logToFile)
    {
        setenv("LOOL_LOGFILE", "1", true);
        const auto it = logProperties.find("path");
        if (it != logProperties.end())
        {
            setenv("LOOL_LOGFILENAME", it->second.c_str(), true);
#if ENABLE_DEBUG
            std::cerr << "\nFull log is available in: " << it->second.c_str() << std::endl;
#endif
        }
    }

    Log::initialize("wsd", logLevel, withColor, logToFile, logProperties);

#if ENABLE_SSL
    LOOLWSD::SSLEnabled.set(getConfigValue<bool>(conf, "ssl.enable", true));
#else
    LOOLWSD::SSLEnabled.set(false);
#endif

    if (LOOLWSD::isSSLEnabled())
    {
        LOG_INF("SSL support: SSL is enabled.");
    }
    else
    {
        LOG_WRN("SSL support: SSL is disabled.");
    }

#if ENABLE_SSL
    LOOLWSD::SSLTermination.set(getConfigValue<bool>(conf, "ssl.termination", true));
#else
    LOOLWSD::SSLTermination.set(false);
#endif

    Cache = getPathFromConfig("tile_cache_path");
    SysTemplate = getPathFromConfig("sys_template_path");
    LoTemplate = getPathFromConfig("lo_template_path");
    ChildRoot = getPathFromConfig("child_root_path");
    ServerName = config().getString("server_name");

    FileServerRoot = getPathFromConfig("file_server_root_path");
    NumPreSpawnedChildren = getConfigValue<int>(conf, "num_prespawn_children", 1);
    if (NumPreSpawnedChildren < 1)
    {
        LOG_WRN("Invalid num_prespawn_children in config (" << NumPreSpawnedChildren << "). Resetting to 1.");
        NumPreSpawnedChildren = 1;
    }

    const auto maxConcurrency = getConfigValue<int>(conf, "per_document.max_concurrency", 4);
    if (maxConcurrency > 0)
    {
        setenv("MAX_CONCURRENCY", std::to_string(maxConcurrency).c_str(), 1);
    }

    // Otherwise we profile the soft-device at jail creation time.
    setenv("SAL_DISABLE_OPENCL", "true", 1);

    // Log the connection and document limits.
    static_assert(MAX_CONNECTIONS >= 3, "MAX_CONNECTIONS must be at least 3");
    static_assert(MAX_DOCUMENTS > 0 && MAX_DOCUMENTS <= MAX_CONNECTIONS, "MAX_DOCUMENTS must be positive and no more than MAX_CONNECTIONS");
    LOG_INF("Maximum concurrent open Documents limit: " << MAX_DOCUMENTS);
    LOG_INF("Maximum concurrent client Connections limit: " << MAX_CONNECTIONS);

    LOOLWSD::NumConnections = 0;

    // Command Tracing.
    if (getConfigValue<bool>(conf, "trace[@enable]", false))
    {
        const auto& path = getConfigValue<std::string>(conf, "trace.path", "");
        const auto recordOutgoing = getConfigValue<bool>(conf, "trace.outgoing.record", false);
        std::vector<std::string> filters;
        for (size_t i = 0; ; ++i)
        {
            const std::string confPath = "trace.filter.message[" + std::to_string(i) + "]";
            const auto regex = config().getString(confPath, "");
            if (!regex.empty())
            {
                filters.push_back(regex);
            }
            else if (!config().has(confPath))
            {
                break;
            }
        }

        const auto compress = getConfigValue<bool>(conf, "trace.path[@compress]", false);
        const auto takeSnapshot = getConfigValue<bool>(conf, "trace.path[@snapshot]", false);
        TraceDumper.reset(new TraceFileWriter(path, recordOutgoing, compress, takeSnapshot, filters));
        LOG_INF("Command trace dumping enabled to file: " << path);
    }

    StorageBase::initialize();

    ServerApplication::initialize(self);

#if ENABLE_DEBUG
    std::cerr << "\nLaunch this in your browser:\n\n"
              << getLaunchURI() << '\n' << std::endl;

    std::string adminURI = getAdminURI(config());
    if (!adminURI.empty())
        std::cerr << "\nOr for the Admin Console:\n\n"
                  << adminURI << '\n' << std::endl;
#endif
}

void LOOLWSD::initializeSSL()
{
    if (!LOOLWSD::isSSLEnabled())
    {
        return;
    }

    const auto ssl_cert_file_path = getPathFromConfig("ssl.cert_file_path");
    LOG_INF("SSL Cert file: " << ssl_cert_file_path);

    const auto ssl_key_file_path = getPathFromConfig("ssl.key_file_path");
    LOG_INF("SSL Key file: " << ssl_key_file_path);

    const auto ssl_ca_file_path = getPathFromConfig("ssl.ca_file_path");
    LOG_INF("SSL CA file: " << ssl_ca_file_path);

#if ENABLE_SSL
    // Initialize the non-blocking socket SSL.
    SslContext::initialize(ssl_cert_file_path,
                           ssl_key_file_path,
                           ssl_ca_file_path);
#endif

    Poco::Crypto::initializeCrypto();

    Poco::Net::initializeSSL();
    Poco::Net::Context::Params sslParams;
    sslParams.certificateFile = ssl_cert_file_path;
    sslParams.privateKeyFile = ssl_key_file_path;
    sslParams.caLocation = ssl_ca_file_path;
    // Don't ask clients for certificate
    sslParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    // FIXME: ConsoleCertificateHandler will block on stdin upon error!
    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleHandler = new Poco::Net::KeyConsoleHandler(true);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidCertHandler = new Poco::Net::ConsoleCertificateHandler(true);

    Poco::Net::Context::Ptr sslContext = new Poco::Net::Context(Poco::Net::Context::SERVER_USE, sslParams);
    Poco::Net::SSLManager::instance().initializeServer(consoleHandler, invalidCertHandler, sslContext);

    // Init client
    Poco::Net::Context::Params sslClientParams;
    // TODO: Be more strict and setup SSL key/certs for owncloud server and us
    sslClientParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleClientHandler = new Poco::Net::KeyConsoleHandler(false);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidClientCertHandler = new Poco::Net::AcceptCertificateHandler(false);

    Poco::Net::Context::Ptr sslClientContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslClientParams);
    Poco::Net::SSLManager::instance().initializeClient(consoleClientHandler, invalidClientCertHandler, sslClientContext);
}

void LOOLWSD::dumpNewSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri, const std::string& path)
{
    if (TraceDumper)
    {
        try
        {
            TraceDumper->newSession(id, sessionId, uri, path);
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception in tracer newSession: " << exc.what());
        }
    }
}

void LOOLWSD::dumpEndSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri)
{
    if (TraceDumper)
    {
        try
        {
            TraceDumper->endSession(id, sessionId, uri);
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception in tracer newSession: " << exc.what());
        }
    }
}

void LOOLWSD::dumpEventTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeEvent(id, sessionId, data);
    }
}

void LOOLWSD::dumpIncomingTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeIncoming(id, sessionId, data);
    }
}

void LOOLWSD::dumpOutgoingTrace(const std::string& id, const std::string& sessionId, const std::string& data)
{
    if (TraceDumper)
    {
        TraceDumper->writeOutgoing(id, sessionId, data);
    }
}

void LOOLWSD::defineOptions(OptionSet& optionSet)
{
    ServerApplication::defineOptions(optionSet);

    optionSet.addOption(Option("help", "", "Display help information on command line arguments.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("version", "", "Display version information.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("port", "", "Port number to listen to (default: " +
                               std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "),"
                               " must not be " + std::to_string(MasterPortNumber) + ".")
                        .required(false)
                        .repeatable(false)
                        .argument("port_number"));

    optionSet.addOption(Option("disable-ssl", "", "Disable SSL security layer.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("override", "o", "Override any setting by providing fullxmlpath=value.")
                        .required(false)
                        .repeatable(true)
                        .argument("xmlpath"));

    optionSet.addOption(Option("config-file", "", "Override configuration file path.")
                        .required(false)
                        .repeatable(false)
                        .argument("path"));

#if ENABLE_DEBUG
    optionSet.addOption(Option("unitlib", "", "Unit testing library path.")
                        .required(false)
                        .repeatable(false)
                        .argument("unitlib"));

    optionSet.addOption(Option("nocaps", "", "Use a non-privileged forkit for valgrinding.")
                        .required(false)
                        .repeatable(false));

    optionSet.addOption(Option("careerspan", "", "How many seconds to run.")
                        .required(false)
                        .repeatable(false)
                        .argument("seconds"));
#endif

#ifdef FUZZER
    optionSet.addOption(Option("dummy-lok", "", "Use empty (dummy) LibreOfficeKit implementation instead a real LibreOffice.")
                        .required(false)
                        .repeatable(false));
    optionSet.addOption(Option("fuzz", "", "Read input from the specified file for fuzzing.")
                        .required(false)
                        .repeatable(false)
                        .argument("trace_file_name"));
#endif
}

void LOOLWSD::handleOption(const std::string& optionName,
                           const std::string& value)
{
    ServerApplication::handleOption(optionName, value);

    if (optionName == "help")
    {
        displayHelp();
        std::exit(Application::EXIT_OK);
    }
    else if (optionName == "version")
        DisplayVersion = true;
    else if (optionName == "port")
        ClientPortNumber = std::stoi(value);
    else if (optionName == "disable-ssl")
        _overrideSettings["ssl.enable"] = "false";
    else if (optionName == "override")
    {
        std::string optName;
        std::string optValue;
        LOOLProtocol::parseNameValuePair(value, optName, optValue);
        _overrideSettings[optName] = optValue;
    }
    else if (optionName == "config-file")
        ConfigFile = value;
#if ENABLE_DEBUG
    else if (optionName == "unitlib")
        UnitTestLibrary = value;
#ifndef KIT_IN_PROCESS
    else if (optionName == "nocaps")
        NoCapsForKit = true;
#endif
    else if (optionName == "careerspan")
        careerSpanSeconds = std::stoi(value);

    static const char* clientPort = std::getenv("LOOL_TEST_CLIENT_PORT");
    if (clientPort)
        ClientPortNumber = std::stoi(clientPort);

    static const char* masterPort = std::getenv("LOOL_TEST_MASTER_PORT");
    if (masterPort)
        MasterPortNumber = std::stoi(masterPort);
#endif

#ifdef FUZZER
    if (optionName == "dummy-lok")
        DummyLOK = true;
    else if (optionName == "fuzz")
        FuzzFileName = value;
#endif
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice Online WebSocket server.");
    helpFormatter.format(std::cout);
}

bool LOOLWSD::checkAndRestoreForKit()
{
#ifdef KIT_IN_PROCESS
    return false;
#else

    if (ForKitProcId == -1)
    {
        // Fire the ForKit process for the first time.
        if (!createForKit())
        {
            // Should never fail.
            LOG_FTL("Failed to spawn loolforkit.");
            return Application::EXIT_SOFTWARE;
        }
    }

    int status;
    const pid_t pid = waitpid(ForKitProcId, &status, WUNTRACED | WNOHANG);
    if (pid > 0)
    {
        if (pid == ForKitProcId)
        {
            if (WIFEXITED(status) || WIFSIGNALED(status))
            {
                if (WIFEXITED(status))
                {
                    LOG_INF("Forkit process [" << pid << "] exited with code: " <<
                            WEXITSTATUS(status) << ".");
                }
                else
                {
                    LOG_ERR("Forkit process [" << pid << "] " <<
                            (WCOREDUMP(status) ? "core-dumped" : "died") <<
                            " with " << SigUtil::signalName(WTERMSIG(status)));
                }

                // Spawn a new forkit and try to dust it off and resume.
                if (!createForKit())
                {
                    LOG_FTL("Failed to spawn forkit instance. Shutting down.");
                    SigUtil::requestShutdown();
                }
            }
            else if (WIFSTOPPED(status))
            {
                LOG_INF("Forkit process [" << pid << "] stopped with " <<
                        SigUtil::signalName(WSTOPSIG(status)));
            }
            else if (WIFCONTINUED(status))
            {
                LOG_INF("Forkit process [" << pid << "] resumed with SIGCONT.");
            }
            else
            {
                LOG_WRN("Unknown status returned by waitpid: " << std::hex << status << ".");
            }

            return true;
        }
        else
        {
            LOG_ERR("An unknown child process [" << pid << "] died.");
        }
    }
    else if (pid < 0)
    {
        LOG_SYS("Forkit waitpid failed.");
        if (errno == ECHILD)
        {
            // No child processes.
            // Spawn a new forkit and try to dust it off and resume.
            if (!createForKit())
            {
                LOG_FTL("Failed to spawn forkit instance. Shutting down.");
                SigUtil::requestShutdown();
            }
        }

        return true;
    }

    return false;
#endif
}

void PrisonerPoll::wakeupHook()
{
    /// FIXME: we should do this less frequently
    /// currently the prisoner poll wakes up quite
    /// a lot.
    if (!LOOLWSD::checkAndRestoreForKit())
    {
        // No children have died.
        // Make sure we have sufficient reserves.
        if (prespawnChildren())
        {
            // Nothing more to do this round, unless we are fuzzing
#if FUZZER
            if (!LOOLWSD::FuzzFileName.empty())
            {
                std::unique_ptr<Replay> replay(new Replay(
#if ENABLE_SSL
                        "https://127.0.0.1:" + std::to_string(ClientPortNumber),
#else
                        "http://127.0.0.1:" + std::to_string(ClientPortNumber),
#endif
                        LOOLWSD::FuzzFileName));

                std::unique_ptr<Thread> replayThread(new Thread());
                replayThread->start(*replay);

                // block until the replay finishes
                replayThread->join();

                TerminationFlag = true;
            }
#endif
        }
    }
}

bool LOOLWSD::createForKit()
{
#ifdef KIT_IN_PROCESS
    return true;
#else
    LOG_INF("Creating new forkit process.");

    Process::Args args;
    args.push_back("--losubpath=" + std::string(LO_JAIL_SUBPATH));
    args.push_back("--systemplate=" + SysTemplate);
    args.push_back("--lotemplate=" + LoTemplate);
    args.push_back("--childroot=" + ChildRoot);
    args.push_back("--clientport=" + std::to_string(ClientPortNumber));
    args.push_back("--masterport=" + std::to_string(MasterPortNumber));
    if (UnitWSD::get().hasKitHooks())
    {
        args.push_back("--unitlib=" + UnitTestLibrary);
    }

    if (DisplayVersion)
    {
        args.push_back("--version");
    }

    std::string forKitPath = Path(Application::instance().commandPath()).parent().toString() + "loolforkit";
    if (NoCapsForKit)
    {
        forKitPath = forKitPath + std::string("-nocaps");
        args.push_back("--nocaps");
    }

    // If we're recovering forkit, don't allow processing new requests.
    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
    std::unique_lock<std::mutex> newChildrenLock(NewChildrenMutex);

    // Always reap first, in case we haven't done so yet.
    int status;
    waitpid(ForKitProcId, &status, WUNTRACED | WNOHANG);
    ForKitProcId = -1;
    Admin::instance().setForKitPid(ForKitProcId);

    const int oldForKitWritePipe = ForKitWritePipe;
    ForKitWritePipe = -1;
    close(oldForKitWritePipe);

    // ForKit always spawns one.
    ++OutstandingForks;

    LOG_INF("Launching forkit process: " << forKitPath << ' ' <<
            Poco::cat(std::string(" "), args.begin(), args.end()));

    LastForkRequestTime = std::chrono::steady_clock::now();
    Pipe inPipe;
    ProcessHandle child = Process::launch(forKitPath, args, &inPipe, nullptr, nullptr);

    // The Pipe dtor closes the fd, so dup it.
    ForKitWritePipe = dup(inPipe.writeHandle());

    ForKitProcId = child.id();

    LOG_INF("Forkit process launched: " << ForKitProcId);

    // Init the Admin manager
    Admin::instance().setForKitPid(ForKitProcId);

    // Wake the prisoner poll to spawn some children, if necessary.
    PrisonerPoll.wakeup();
    // FIXME: horrors with try_lock in prespawnChildren ...

    return (ForKitProcId != -1);
#endif
}

#ifdef FUZZER
std::mutex Connection::Mutex;
#endif

static std::shared_ptr<DocumentBroker> createDocBroker(WebSocketHandler& ws,
                                                       const std::string& uri,
                                                       const std::string& docKey,
                                                       const Poco::URI& uriPublic)
{
    Util::assertIsLocked(DocBrokersMutex);

    static_assert(MAX_DOCUMENTS > 0, "MAX_DOCUMENTS must be positive");
    if (DocBrokers.size() + 1 > MAX_DOCUMENTS)
    {
        LOG_ERR("Maximum number of open documents reached.");
        shutdownLimitReached(ws);
        return nullptr;
    }

    // Set the one we just created.
    LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
    auto docBroker = DocumentBroker::create(uri, uriPublic, docKey, LOOLWSD::ChildRoot);
    DocBrokers.emplace(docKey, docBroker);
    LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after inserting [" << docKey << "].");

    return docBroker;
}

/// Find the DocumentBroker for the given docKey, if one exists.
/// Otherwise, creates and adds a new one to DocBrokers.
/// May return null if terminating or MaxDocuments limit is reached.
/// After returning a valid instance DocBrokers must be cleaned up after exceptions.
static std::shared_ptr<DocumentBroker> findOrCreateDocBroker(WebSocketHandler& ws,
                                                             const std::string& uri,
                                                             const std::string& docKey,
                                                             const std::string& id,
                                                             const Poco::URI& uriPublic)
{
    LOG_INF("Find or create DocBroker for docKey [" << docKey <<
            "] for session [" << id << "] on url [" << uriPublic.toString() << "].");

    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);

    cleanupDocBrokers();

    if (TerminationFlag)
    {
        LOG_ERR("Termination flag set. No loading new session [" << id << "]");
        return nullptr;
    }

    std::shared_ptr<DocumentBroker> docBroker;

    // Lookup this document.
    auto it = DocBrokers.find(docKey);
    if (it != DocBrokers.end() && it->second)
    {
        // Get the DocumentBroker from the Cache.
        LOG_DBG("Found DocumentBroker with docKey [" << docKey << "].");
        docBroker = it->second;
        if (docBroker->isMarkedToDestroy())
        {
            // Let the waiting happen in parallel to new requests.
            docBrokersLock.unlock();

            // If this document is going out, wait.
            LOG_DBG("Document [" << docKey << "] is marked to destroy, waiting to reload.");

            // FIXME: - easiest to send a fast message to the
            //          client to wait & retry in a bit ...

#if 0 // loolnb
            bool timedOut = true;
            for (size_t i = 0; i < COMMAND_TIMEOUT_MS / POLL_TIMEOUT_MS; ++i)
            {

                // FIXME: blocks !
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_TIMEOUT_MS));

                docBrokersLock.lock();
                it = DocBrokers.find(docKey);
                if (it == DocBrokers.end())
                {
                    // went away successfully
                    docBroker.reset();
                    docBrokersLock.unlock();
                    timedOut = false;
                    break;
                }
                else if (it->second && !it->second->isMarkedToDestroy())
                {
                    // was actually replaced by a real document
                    docBroker = it->second;
                    docBrokersLock.unlock();
                    timedOut = false;
                    break;
                }

                docBrokersLock.unlock();
                if (TerminationFlag)
                {
                    LOG_ERR("Termination flag set. Not loading new session [" << id << "]");
                    return nullptr;
                }
            }

            if (timedOut)
            {
                // Still here, but marked to destroy. Proceed and hope to recover.
                LOG_ERR("Timed out while waiting for document to unload before loading.");
            }
#endif
        }
    }
    else
    {
        LOG_DBG("No DocumentBroker with docKey [" << docKey << "] found. New Child and Document.");
    }

    if (TerminationFlag)
    {
        LOG_ERR("Termination flag set. No loading new session [" << id << "]");
        return nullptr;
    }

    // Indicate to the client that we're connecting to the docbroker.
    const std::string statusConnect = "statusindicator: connect";
    LOG_TRC("Sending to Client [" << statusConnect << "].");
    ws.sendFrame(statusConnect);

    if (!docBroker)
        docBroker = createDocBroker(ws, uri, docKey, uriPublic);

    return docBroker;
}

/// Remove DocumentBroker session and instance from DocBrokers.
static void removeDocBrokerSession(const std::shared_ptr<DocumentBroker>& docBroker, const std::string& id = "")
{
    LOG_CHECK_RET(docBroker && "Null docBroker instance", );

    const auto docKey = docBroker->getDocKey();
    LOG_DBG("Removing docBroker [" << docKey << "]" << (id.empty() ? "" : (" and session [" + id + "].")));

    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
    auto lock = docBroker->getLock();

    if (!id.empty())
        docBroker->removeSession(id);

    if (docBroker->getSessionsCount() == 0 || !docBroker->isAlive())
    {
        LOG_INF("Removing unloaded DocumentBroker for docKey [" << docKey << "].");
        DocBrokers.erase(docKey);
        docBroker->terminateChild(lock, "");
    }
}

static std::shared_ptr<ClientSession> createNewClientSession(const WebSocketHandler& ws,
                                                             const std::string& id,
                                                             const Poco::URI& uriPublic,
                                                             const std::shared_ptr<DocumentBroker>& docBroker,
                                                             const bool isReadOnly)
{
    LOG_CHECK_RET(docBroker && "Null docBroker instance", nullptr);
    try
    {
        auto lock = docBroker->getLock();

        // Validate the broker.
        if (!docBroker->isAlive())
        {
            LOG_ERR("DocBroker is invalid or premature termination of child process.");
            lock.unlock();
            removeDocBrokerSession(docBroker);
            return nullptr;
        }

        if (docBroker->isMarkedToDestroy())
        {
            LOG_ERR("DocBroker is marked to destroy, can't add session.");
            lock.unlock();
            removeDocBrokerSession(docBroker);
            return nullptr;
        }

        // Now we have a DocumentBroker and we're ready to process client commands.
        const std::string statusReady = "statusindicator: ready";
        LOG_TRC("Sending to Client [" << statusReady << "].");
        ws.sendFrame(statusReady);

        // In case of WOPI, if this session is not set as readonly, it might be set so
        // later after making a call to WOPI host which tells us the permission on files
        // (UserCanWrite param).
        auto session = std::make_shared<ClientSession>(id, docBroker, uriPublic, isReadOnly);

        docBroker->queueSession(session);

        lock.unlock();

        const std::string fs = FileUtil::checkDiskSpaceOnRegisteredFileSystems();
        if (!fs.empty())
        {
            LOG_WRN("File system of [" << fs << "] is dangerously low on disk space.");
            const std::string diskfullMsg = "error: cmd=internal kind=diskfull";
            // Alert all other existing sessions also
            Util::alertAllUsers(diskfullMsg);
        }

        return session;
    }
    catch (const std::exception& exc)
    {
        LOG_WRN("Exception while preparing session [" << id << "]: " << exc.what());
        removeDocBrokerSession(docBroker, id);
    }

    return nullptr;
}

class PrisonerRequestDispatcher : public WebSocketHandler
{
    std::weak_ptr<ChildProcess> _childProcess;
public:
    PrisonerRequestDispatcher()
    {
    }
    ~PrisonerRequestDispatcher()
    {
        // Notify the broker that we're done.
        auto child = _childProcess.lock();
        auto docBroker = child ? child->_docBroker.lock() : nullptr;
        if (docBroker)
        {
            // FIXME: No need to notify if asked to stop.
            docBroker->childSocketTerminated();
        }
    }

private:
    /// Keep our socket around ...
    void onConnect(const std::weak_ptr<StreamSocket>& socket) override
    {
        LOG_TRC("Prisoner - new socket\n");
        _socket = socket;
    }

    void onDisconnect() override
    {
        LOG_TRC("Prisoner connection disconnected\n");
    }

    /// Called after successful socket reads.
    void handleIncomingMessage() override
    {
        if (_childProcess.lock())
        {
            // FIXME: inelegant etc. - derogate to websocket code
            WebSocketHandler::handleIncomingMessage();
            return;
        }

        auto socket = _socket.lock();
        std::vector<char>& in = socket->_inBuffer;

        // Find the end of the header, if any.
        static const std::string marker("\r\n\r\n");
        auto itBody = std::search(in.begin(), in.end(),
                                  marker.begin(), marker.end());
        if (itBody == in.end())
        {
            LOG_TRC("#" << socket->getFD() << " doesn't have enough data yet.");
            return;
        }

        // Skip the marker.
        itBody += marker.size();

        Poco::MemoryInputStream message(&in[0], in.size());
        Poco::Net::HTTPRequest request;
        try
        {
            request.read(message);

            auto logger = Log::info();
            // logger << "Request from " << request.clientAddress().toString() << ": "
            logger << "Prisoner request : "
                   << request.getMethod() << " " << request.getURI() << " "
                   << request.getVersion();

            for (const auto& it : request)
            {
                logger << " / " << it.first << ": " << it.second;
            }

            logger << Log::end;

            LOG_TRC("Child connection with URI [" << request.getURI() << "].");
            if (request.getURI().find(NEW_CHILD_URI) != 0)
            {
                LOG_ERR("Invalid incoming URI.");
                return;
            }

            // New Child is spawned.
            const auto params = Poco::URI(request.getURI()).getQueryParameters();
            Poco::Process::PID pid = -1;
            for (const auto& param : params)
            {
                if (param.first == "pid")
                {
                    pid = std::stoi(param.second);
                }
                else if (param.first == "version")
                {
                    LOOLWSD::LOKitVersion = param.second;
                }
            }

            if (pid <= 0)
            {
                LOG_ERR("Invalid PID in child URI [" << request.getURI() << "].");
                return;
            }

            LOG_INF("New child [" << pid << "].");

            // FIXME:
            /* if (UnitWSD::get().filterHandleRequest(
               UnitWSD::TestRequest::Prisoner,
               request, response))
               return; */

            auto child = std::make_shared<ChildProcess>(pid, socket, request);
            _childProcess = child; // weak
            addNewChild(child);

            // Remove from prisoner poll since there is no activity
            // until we attach the childProcess (with this socket)
            // to a docBroker, which will do the polling.
            PrisonerPoll.releaseSocket(socket);

            in.clear();
        }
        catch (const std::exception& exc)
        {
            // Probably don't have enough data just yet.
            // TODO: timeout if we never get enough.
            return;
        }
    }

    /// Prisoner websocket fun ... (for now)
    virtual void handleMessage(bool /*fin*/, WSOpCode /* code */, std::vector<char> &data)
    {
        if (UnitWSD::get().filterChildMessage(data))
            return;

        LOG_TRC("Prisoner message [" << getAbbreviatedMessage(&data[0], data.size()) << "].");

        auto child = _childProcess.lock();
        auto docBroker = child ? child->_docBroker.lock() : nullptr;
        if (docBroker)
        {
            // We should never destroy the broker, since
            // it owns us and will wait on this thread.
            // FIXME: loolnb - check that comment !
            assert(docBroker.use_count() > 1);
            docBroker->handleInput(data);
            return;
        }

        LOG_WRN("Child " << child->_pid <<
                " has no DocumentBroker to handle message: [" <<
                LOOLProtocol::getAbbreviatedMessage(data) << "].");
    }

    bool hasQueuedWrites() const override
    {
        LOG_TRC("PrisonerRequestDispatcher - asked for queued writes");
        return false;
    }

    void performWrites() override
    {
    }
};

/// Handles incoming connections and dispatches to the appropriate handler.
class ClientRequestDispatcher : public SocketHandlerInterface
{
public:
    ClientRequestDispatcher()
    {
    }

private:

    /// Set the socket associated with this ResponseClient.
    void onConnect(const std::weak_ptr<StreamSocket>& socket) override
    {
        _id = LOOLWSD::GenSessionId();
        _connectionNum = ++LOOLWSD::NumConnections;
        LOG_TRC("Connected connection #" << _connectionNum << " of " <<
                MAX_CONNECTIONS << " max as session [" << _id << "].");

        _socket = socket;
    }

    void onDisconnect() override
    {
        if (_clientSession)
            disposeSession();

        const size_t curConnections = --LOOLWSD::NumConnections;
        LOG_TRC("Disconnected connection #" << _connectionNum << " of " <<
                (curConnections + 1) << " existing as session [" << _id << "].");
    }

    /// Called after successful socket reads.
    void handleIncomingMessage() override
    {
        if (_clientSession)
        {
            LOG_INF("Forwarding incoming message to client [" << _id << "]");

            // TODO: might be better to reset the handler in the socket
            // so we avoid this double-dispatching.
            _clientSession->handleIncomingMessage();
            return;
        }

        auto socket = _socket.lock();
        std::vector<char>& in = socket->_inBuffer;

        // Find the end of the header, if any.
        static const std::string marker("\r\n\r\n");
        auto itBody = std::search(in.begin(), in.end(),
                                  marker.begin(), marker.end());
        if (itBody == in.end())
        {
            LOG_TRC("#" << socket->getFD() << " doesn't have enough data yet.");
            return;
        }

        // Skip the marker.
        itBody += marker.size();

        Poco::MemoryInputStream message(&in[0], in.size());
        Poco::Net::HTTPRequest request;
        try
        {
            request.read(message);

            auto logger = Log::info();
            // logger << "Request from " << request.clientAddress().toString() << ": "
            logger << "Request : "
                   << request.getMethod() << " " << request.getURI() << " "
                   << request.getVersion();

            for (const auto& it : request)
            {
                logger << " / " << it.first << ": " << it.second;
            }

            logger << Log::end;

            const std::streamsize contentLength = request.getContentLength();
            const auto offset = itBody - in.begin();
            const std::streamsize available = in.size() - offset;

            if (contentLength != Poco::Net::HTTPMessage::UNKNOWN_CONTENT_LENGTH && available < contentLength)
            {
                LOG_DBG("Not enough content yet: ContentLength: " << contentLength << ", available: " << available);
                return;
            }
        }
        catch (const std::exception& exc)
        {
            // Probably don't have enough data just yet.
            // TODO: timeout if we never get enough.
            return;
        }

        try
        {
            // Routing
            Poco::URI requestUri(request.getURI());
            std::vector<std::string> reqPathSegs;
            requestUri.getPathSegments(reqPathSegs);

            // File server
            if (reqPathSegs.size() >= 1 && reqPathSegs[0] == "loleaflet")
            {
                handleFileServerRequest(request, message);
            }
            // Admin connections
            else if (reqPathSegs.size() >= 2 && reqPathSegs[0] == "lool" && reqPathSegs[1] == "adminws")
            {
                handleAdminRequest(request);
            }
            // Client post and websocket connections
            else if ((request.getMethod() == HTTPRequest::HTTP_GET ||
                      request.getMethod() == HTTPRequest::HTTP_HEAD) &&
                     request.getURI() == "/")
            {
                handleRootRequest(request);
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET && request.getURI() == "/favicon.ico")
            {
                handleFaviconRequest(request);
            }
            else if (request.getMethod() == HTTPRequest::HTTP_GET && request.getURI() == "/hosting/discovery")
            {
                handleWopiDiscoveryRequest(request);
            }
            else
            {
                StringTokenizer reqPathTokens(request.getURI(), "/?", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);
                if (!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0) &&
                    reqPathTokens.count() > 0 && reqPathTokens[0] == "lool")
                {
                    // All post requests have url prefix 'lool'.
                    handlePostRequest_Blocks(request, message);
                }
                else if (reqPathTokens.count() > 2 && reqPathTokens[0] == "lool" && reqPathTokens[2] == "ws")
                {
                    handleClientWsUpgrade(request, reqPathTokens[1]);
                }
                else
                {
                    LOG_ERR("Unknown resource: " << request.getURI());

                    // Bad request.
                    std::ostringstream oss;
                    oss << "HTTP/1.1 400\r\n"
                        << "Date: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
                        << "User-Agent: LOOLWSD WOPI Agent\r\n"
                        << "Content-Length: 0\r\n"
                        << "\r\n";
                    socket->sendHttpResponse(oss.str());
                    socket->shutdown();
                }
            }

            // if we succeeded - remove the request from our input buffer
            // we expect one request per socket
            in.clear();
        }
        catch (const std::exception& exc)
        {
            // TODO: Send back failure.
            // NOTE: Check _wsState to choose between HTTP response or WebSocket (app-level) error.
        }
    }

    bool hasQueuedWrites() const override
    {
        // FIXME: - the session should be owning the fd in DocumentBroker's _poll
        if (_clientSession)
            return _clientSession->hasQueuedWrites();

        LOG_TRC("ClientRequestDispatcher - asked for queued writes");
        return false;
    }

    void performWrites() override
    {
        // FIXME: - the session should be owning the fd in DocumentBroker's _poll
        if (_clientSession)
            return _clientSession->performWrites();
    }

    void handleFileServerRequest(const Poco::Net::HTTPRequest& request, Poco::MemoryInputStream& message)
    {
        auto socket = _socket.lock();
        FileServerRequestHandler::handleRequest(request, message, socket);
        socket->shutdown();
    }

    void handleAdminRequest(const Poco::Net::HTTPRequest& request)
    {
        LOG_ERR("Admin request: " << request.getURI());
        // FIXME: implement admin support.
    }

    void handleRootRequest(const Poco::Net::HTTPRequest& request)
    {
        LOG_DBG("HTTP request: " << request.getURI());
        const std::string mimeType = "text/plain";
        const std::string responseString = "OK";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
            << "User-Agent: LOOLWSD WOPI Agent\r\n"
            << "Content-Length: " << responseString.size() << "\r\n"
            << "Content-Type: " << mimeType << "\r\n"
            << "\r\n";

        if (request.getMethod() == Poco::Net::HTTPRequest::HTTP_GET)
        {
            oss << responseString;
        }

        auto socket = _socket.lock();
        socket->sendHttpResponse(oss.str());
        socket->shutdown();
        LOG_INF("Sent / response successfully.");
    }

    void handleFaviconRequest(const Poco::Net::HTTPRequest& request)
    {
        LOG_DBG("Favicon request: " << request.getURI());
        std::string mimeType = "image/vnd.microsoft.icon";
        std::string faviconPath = Path(Application::instance().commandPath()).parent().toString() + "favicon.ico";
        if (!File(faviconPath).exists())
        {
            faviconPath = LOOLWSD::FileServerRoot + "/favicon.ico";
        }

        auto socket = _socket.lock();
        HttpHelper::sendFile(socket, faviconPath, mimeType);
        socket->shutdown();
    }

    void handleWopiDiscoveryRequest(const Poco::Net::HTTPRequest& request)
    {
        LOG_DBG("Wopi discovery request: " << request.getURI());

        // http://server/hosting/discovery
        std::string discoveryPath = Path(Application::instance().commandPath()).parent().toString() + "discovery.xml";
        if (!File(discoveryPath).exists())
        {
            discoveryPath = LOOLWSD::FileServerRoot + "/discovery.xml";
        }

        const std::string mediaType = "text/xml";
        const std::string action = "action";
        const std::string urlsrc = "urlsrc";
        const auto& config = Application::instance().config();
        const std::string loleafletHtml = config.getString("loleaflet_html", "loleaflet.html");
        const std::string uriValue = ((LOOLWSD::isSSLEnabled() || LOOLWSD::isSSLTermination()) ? "https://" : "http://")
                                   + (LOOLWSD::ServerName.empty() ? request.getHost() : LOOLWSD::ServerName)
                                   + "/loleaflet/" LOOLWSD_VERSION_HASH "/" + loleafletHtml + '?';

        InputSource inputSrc(discoveryPath);
        DOMParser parser;
        AutoPtr<Poco::XML::Document> docXML = parser.parse(&inputSrc);
        AutoPtr<NodeList> listNodes = docXML->getElementsByTagName(action);

        for (unsigned long it = 0; it < listNodes->length(); ++it)
        {
            static_cast<Element*>(listNodes->item(it))->setAttribute(urlsrc, uriValue);
        }

        std::ostringstream ostrXML;
        DOMWriter writer;
        writer.writeNode(ostrXML, docXML);
        const std::string xml = ostrXML.str();

        // TODO: Refactor this to some common handler.
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Last-Modified: " << Poco::DateTimeFormatter::format(Poco::Timestamp(), Poco::DateTimeFormat::HTTP_FORMAT) << "\r\n"
            << "User-Agent: LOOLWSD WOPI Agent\r\n"
            << "Content-Length: " << xml.size() << "\r\n"
            << "Content-Type: " << mediaType << "\r\n"
            << "\r\n"
            << xml;

        auto socket = _socket.lock();
        socket->sendHttpResponse(oss.str());
        socket->shutdown();
        LOG_INF("Sent discovery.xml successfully.");
    }

    static std::string getContentType(const std::string& fileName)
    {
        const std::string nodePath = Poco::format("//[@ext='%s']", Poco::Path(fileName).getExtension());
        std::string discPath = Path(Application::instance().commandPath()).parent().toString() + "discovery.xml";
        if (!File(discPath).exists())
        {
            discPath = LOOLWSD::FileServerRoot + "/discovery.xml";
        }

        InputSource input(discPath);
        DOMParser domParser;
        AutoPtr<Poco::XML::Document> doc = domParser.parse(&input);
        // TODO. discovery.xml missing application/pdf
        Node* node = doc->getNodeByPath(nodePath);
        if (node && (node = node->parentNode()) && node->hasAttributes())
        {
            return dynamic_cast<Element*>(node)->getAttribute("name");
        }

        return "application/octet-stream";
    }

    void handlePostRequest_Blocks(const Poco::Net::HTTPRequest& request, Poco::MemoryInputStream& message)
    {
        LOG_INF("Post request: [" << request.getURI() << "]");

        Poco::Net::HTTPResponse response;
        auto socket = _socket.lock();

        StringTokenizer tokens(request.getURI(), "/?");
        if (tokens.count() >= 3 && tokens[2] == "convert-to")
        {
            std::string fromPath;
            ConvertToPartHandler handler(fromPath);
            HTMLForm form(request, message, handler);
            const std::string format = (form.has("format") ? form.get("format") : "");

            bool sent = false;
            if (!fromPath.empty())
            {
                if (!format.empty())
                {
                    LOG_INF("Conversion request for URI [" << fromPath << "].");

                    auto uriPublic = DocumentBroker::sanitizeURI(fromPath);
                    const auto docKey = DocumentBroker::getDocKey(uriPublic);

                    // This lock could become a bottleneck.
                    // In that case, we can use a pool and index by publicPath.
                    std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);

                    LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
                    auto docBroker = DocumentBroker::create(fromPath, uriPublic, docKey, LOOLWSD::ChildRoot);

                    cleanupDocBrokers();

                    // FIXME: What if the same document is already open? Need a fake dockey here?
                    LOG_DBG("New DocumentBroker for docKey [" << docKey << "].");
                    DocBrokers.emplace(docKey, docBroker);
                    LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after inserting [" << docKey << "].");

                    // Load the document.
                    auto session = std::make_shared<ClientSession>(_id, docBroker, uriPublic);

                    auto lock = docBroker->getLock();
                    size_t sessionsCount = docBroker->queueSession(session);
                    lock.unlock();
                    LOG_TRC(docKey << ", ws_sessions++: " << sessionsCount);

                    docBrokersLock.unlock();

                    std::string encodedFrom;
                    URI::encode(docBroker->getPublicUri().getPath(), "", encodedFrom);
                    const std::string load = "load url=" + encodedFrom;
                    std::vector<char> loadRequest(load.begin(), load.end());
                    session->handleMessage(true, WebSocketHandler::WSOpCode::Text, loadRequest);

                    // FIXME: Check for security violations.
                    Path toPath(docBroker->getPublicUri().getPath());
                    toPath.setExtension(format);
                    const std::string toJailURL = "file://" + std::string(JAILED_DOCUMENT_ROOT) + toPath.getFileName();
                    std::string encodedTo;
                    URI::encode(toJailURL, "", encodedTo);

                    // Convert it to the requested format.
                    const auto saveas = "saveas url=" + encodedTo + " format=" + format + " options=";
                    std::vector<char> saveasRequest(saveas.begin(), saveas.end());
                    session->handleMessage(true, WebSocketHandler::WSOpCode::Text, saveasRequest);

                    // Send it back to the client.
                    try
                    {
                        Poco::URI resultURL(session->getSaveAsUrl(COMMAND_TIMEOUT_MS));
                        LOG_TRC("Save-as URL: " << resultURL.toString());

                        if (!resultURL.getPath().empty())
                        {
                            const std::string mimeType = "application/octet-stream";
                            std::string encodedFilePath;
                            URI::encode(resultURL.getPath(), "", encodedFilePath);
                            LOG_TRC("Sending file: " << encodedFilePath);
                            HttpHelper::sendFile(socket, encodedFilePath, mimeType);
                            sent = true;
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        LOG_ERR("Failed to get save-as url: " << ex.what());
                    }

                    docBrokersLock.lock();
                    auto docLock = docBroker->getLock();
                    sessionsCount = docBroker->removeSession(_id);
                    if (sessionsCount == 0)
                    {
                        // At this point we're done.
                        LOG_DBG("Removing DocumentBroker for docKey [" << docKey << "].");
                        DocBrokers.erase(docKey);
                        docBroker->terminateChild(docLock, "");
                        LOG_TRC("Have " << DocBrokers.size() << " DocBrokers after removing [" << docKey << "].");
                    }
                    else
                    {
                        LOG_ERR("Multiple sessions during conversion. " << sessionsCount << " sessions remain.");
                    }
                }

                // Clean up the temporary directory the HTMLForm ctor created.
                Path tempDirectory(fromPath);
                tempDirectory.setFileName("");
                FileUtil::removeFile(tempDirectory, /*recursive=*/true);
            }

            if (!sent)
            {
                // TODO: We should differentiate between bad request and failed conversion.
                throw BadRequestException("Failed to convert and send file.");
            }

            return;
        }
        else if (tokens.count() >= 4 && tokens[3] == "insertfile")
        {
            LOG_INF("Insert file request.");
            response.set("Access-Control-Allow-Origin", "*");
            response.set("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.set("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");

            std::string tmpPath;
            ConvertToPartHandler handler(tmpPath);
            HTMLForm form(request, message, handler);

            if (form.has("childid") && form.has("name"))
            {
                const std::string formChildid(form.get("childid"));
                const std::string formName(form.get("name"));

                // Validate the docKey
                std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
                std::string decodedUri;
                URI::decode(tokens[2], decodedUri);
                const auto docKey = DocumentBroker::getDocKey(DocumentBroker::sanitizeURI(decodedUri));
                auto docBrokerIt = DocBrokers.find(docKey);

                // Maybe just free the client from sending childid in form ?
                if (docBrokerIt == DocBrokers.end() || docBrokerIt->second->getJailId() != formChildid)
                {
                    throw BadRequestException("DocKey [" + docKey + "] or childid [" + formChildid + "] is invalid.");
                }
                docBrokersLock.unlock();

                // protect against attempts to inject something funny here
                if (formChildid.find('/') == std::string::npos && formName.find('/') == std::string::npos)
                {
                    LOG_INF("Perform insertfile: " << formChildid << ", " << formName);
                    const std::string dirPath = LOOLWSD::ChildRoot + formChildid
                                              + JAILED_DOCUMENT_ROOT + "insertfile";
                    File(dirPath).createDirectories();
                    std::string fileName = dirPath + "/" + form.get("name");
                    File(tmpPath).moveTo(fileName);
                    response.setContentLength(0);
                    socket->sendHttpResponse(response);
                    return;
                }
            }
        }
        else if (tokens.count() >= 6)
        {
            LOG_INF("File download request.");
            // TODO: Check that the user in question has access to this file!

            // 1. Validate the dockey
            std::string decodedUri;
            URI::decode(tokens[2], decodedUri);
            const auto docKey = DocumentBroker::getDocKey(DocumentBroker::sanitizeURI(decodedUri));
            std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
            auto docBrokerIt = DocBrokers.find(docKey);
            if (docBrokerIt == DocBrokers.end())
            {
                throw BadRequestException("DocKey [" + docKey + "] is invalid.");
            }

            // 2. Cross-check if received child id is correct
            if (docBrokerIt->second->getJailId() != tokens[3])
            {
                throw BadRequestException("ChildId does not correspond to docKey");
            }

            // 3. Don't let user download the file in main doc directory containing
            // the document being edited otherwise we will end up deleting main directory
            // after download finishes
            if (docBrokerIt->second->getJailId() == tokens[4])
            {
                throw BadRequestException("RandomDir cannot be equal to ChildId");
            }
            docBrokersLock.unlock();

            std::string fileName;
            bool responded = false;
            URI::decode(tokens[5], fileName);
            const Path filePath(LOOLWSD::ChildRoot + tokens[3]
                                + JAILED_DOCUMENT_ROOT + tokens[4] + "/" + fileName);
            LOG_INF("HTTP request for: " << filePath.toString());
            if (filePath.isAbsolute() && File(filePath).exists())
            {
                std::string contentType = getContentType(fileName);
                response.set("Access-Control-Allow-Origin", "*");
                if (Poco::Path(fileName).getExtension() == "pdf")
                {
                    contentType = "application/pdf";
                    response.set("Content-Disposition", "attachment; filename=\"" + fileName + "\"");
                }

                try
                {
                    response.setContentType(contentType);
                    HttpHelper::sendFile(socket, filePath.toString(), response);
                    responded = true;
                }
                catch (const Exception& exc)
                {
                    LOG_ERR("Error sending file to client: " << exc.displayText() <<
                            (exc.nested() ? " (" + exc.nested()->displayText() + ")" : ""));
                }

                FileUtil::removeFile(File(filePath.parent()).path(), true);
            }
            else
            {
                LOG_ERR("Download file [" << filePath.toString() << "] not found.");
            }

            (void)responded;
            return; // responded;
        }

        throw BadRequestException("Invalid or unknown request.");
    }

    void handleClientWsUpgrade(const Poco::Net::HTTPRequest& request, const std::string& url)
    {
        // requestHandler = new ClientRequestHandler();
        LOG_INF("Client WS request" << request.getURI() << ", url: " << url);

        // First Upgrade.
        WebSocketHandler ws(_socket, request);

        if (_connectionNum > MAX_CONNECTIONS)
        {
            LOG_ERR("Limit on maximum number of connections of " << MAX_CONNECTIONS << " reached.");
            shutdownLimitReached(ws);
            return;
        }

        LOG_INF("Starting GET request handler for session [" << _id << "] on url [" << url << "].");

        // Indicate to the client that document broker is searching.
        const std::string status("statusindicator: find");
        LOG_TRC("Sending to Client [" << status << "].");
        ws.sendFrame(status);

        const auto uriPublic = DocumentBroker::sanitizeURI(url);
        const auto docKey = DocumentBroker::getDocKey(uriPublic);
        LOG_INF("Sanitized URI [" << url << "] to [" << uriPublic.toString() <<
                "] and mapped to docKey [" << docKey << "] for session [" << _id << "].");

        // Check if readonly session is required
        bool isReadOnly = false;
        for (const auto& param : uriPublic.getQueryParameters())
        {
            LOG_DBG("Query param: " << param.first << ", value: " << param.second);
            if (param.first == "permission" && param.second == "readonly")
            {
                isReadOnly = true;
            }
        }

        LOG_INF("URL [" << url << "] is " << (isReadOnly ? "readonly" : "writable") << ".");

        // Request a kit process for this doc.
        auto docBroker = findOrCreateDocBroker(ws, url, docKey, _id, uriPublic);
        if (docBroker)
        {
            // TODO: Move to DocumentBroker.
            _clientSession = createNewClientSession(ws, _id, uriPublic, docBroker, isReadOnly);
            if (_clientSession)
            {
                // Transfer the socket to the DocumentBroker.
                auto socket = _socket.lock();
                if (socket)
                {
                    WebServerPoll.releaseSocket(socket);
                    _clientSession->onConnect(socket);
                    docBroker->addSocketToPoll(socket);
                }
                docBroker->startThread();
            }
        }
        if (!docBroker || !_clientSession)
            LOG_WRN("Failed to connect DocBroker and Client Session.");
    }

    // this session went away - cleanup now.
    void disposeSession()
    {
        LOG_CHECK_RET(_clientSession && "Null ClientSession instance", );
        const auto docBroker = _clientSession->getDocumentBroker();
        LOG_CHECK_RET(docBroker && "Null DocumentBroker instance", );
        const auto docKey = docBroker->getDocKey();

        try
        {
            // Connection terminated. Destroy session.
            LOG_DBG("Client session [" << _id << "] on docKey [" << docKey << "] terminated. Cleaning up.");


            // We issue a force-save when last editable (non-readonly) session is going away
            const auto sessionsCount = docBroker->removeSession(_id, true);
            if (sessionsCount == 0)
            {
                // We've supposedly destroyed the last session, now cleanup.
                removeDocBrokerSession(docBroker);
            }

            LOG_INF("Finishing GET request handler for session [" << _id << "].");
        }
        catch (const UnauthorizedRequestException& exc)
        {
            LOG_ERR("Error in client request handler: " << exc.toString());
            const std::string status = "error: cmd=internal kind=unauthorized";
            LOG_TRC("Sending to Client [" << status << "].");
            _clientSession->sendFrame(status);
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("Error in client request handler: " << exc.what());
        }

        try
        {
            if (_clientSession->isCloseFrame())
            {
                LOG_TRC("Normal close handshake.");
                // Client initiated close handshake
                // respond with close frame
                _clientSession->shutdown();
            }
            else if (!ShutdownRequestFlag)
            {
                // something wrong, with internal exceptions
                LOG_TRC("Abnormal close handshake.");
                _clientSession->closeFrame();
                _clientSession->shutdown(WebSocketHandler::StatusCodes::ENDPOINT_GOING_AWAY);
            }
            else
            {
#if 0 // loolnb
                std::lock_guard<std::mutex> lock(ClientWebSocketsMutex);
                LOG_TRC("Capturing Client WS for [" << _id << "]");
                // ClientWebSockets.push_back(ws); //FIXME
#endif
            }
        }
        catch (const std::exception& exc)
        {
            LOG_WRN("Exception while closing socket for session [" << _id <<
                    "] of docKey [" << docKey << "]: " << exc.what());
        }
    }

private:
    // The socket that owns us (we can't own it).
    std::weak_ptr<StreamSocket> _socket;
    std::shared_ptr<ClientSession> _clientSession;
    std::string _id;
    size_t _connectionNum;
};


class PlainSocketFactory : public SocketFactory
{
    std::shared_ptr<Socket> create(const int fd) override
    {
        return StreamSocket::create<StreamSocket>(fd, std::unique_ptr<SocketHandlerInterface>{ new ClientRequestDispatcher });
    }
};

#if ENABLE_SSL
class SslSocketFactory : public SocketFactory
{
    std::shared_ptr<Socket> create(const int fd) override
    {
        auto socket = StreamSocket::create<SslStreamSocket>(fd, std::unique_ptr<SocketHandlerInterface>{ new ClientRequestDispatcher });

        // Do the ssl handshake and read the request.
        // TODO is this really necessary?  This goes against how the polling &
        // buffering is done in the generic / non-ssl case...
        socket->readIncomingData();
        return socket;
    }
};
#endif

class PrisonerSocketFactory : public SocketFactory
{
    std::shared_ptr<Socket> create(const int fd) override
    {
        return StreamSocket::create<StreamSocket>(fd, std::unique_ptr<SocketHandlerInterface>{ new PrisonerRequestDispatcher });
    }
};

/// The main server thread.
///
/// Waits for the connections from the loleaflets, and creates the
/// websockethandlers accordingly.
class LOOLWSDServer
{
    LOOLWSDServer(LOOLWSDServer&& other) = delete;
    const LOOLWSDServer& operator=(LOOLWSDServer&& other) = delete;
public:
    LOOLWSDServer() :
        _stop(false),
        _acceptPoll("accept_poll")
    {
    }

    ~LOOLWSDServer()
    {
        stop();
    }

    void startPrisoners(const int port)
    {
        PrisonerPoll.insertNewSocket(findPrisonerServerPort(port));
        PrisonerPoll.startThread();
    }

    void start(const int port)
    {
        _acceptPoll.insertNewSocket(findServerPort(port));
        _acceptPoll.startThread();
        WebServerPoll.startThread();
    }

    void stop()
    {
        _stop = true;
        SocketPoll::wakeupWorld();
    }

    void dumpState()
    {
        std::cerr << "LOOLWSDServer:\n"
                  << "   Ports: server " << ClientPortNumber
                  <<          " prisoner " << MasterPortNumber << "\n"
                  << "  stop: " << _stop << "\n"
                  << "  TerminationFlag: " << TerminationFlag << "\n"
                  << "  isShuttingDown: " << ShutdownRequestFlag << "\n"
                  << "  NewChildren: " << NewChildren.size() << "\n"
                  << "  OutstandingForks: " << OutstandingForks << "\n";

        std::cerr << "Server poll:\n";
        _acceptPoll.dumpState();

        std::cerr << "Web Server poll:\n";
        WebServerPoll.dumpState();

        std::cerr << "Prisoner poll:\n";
        PrisonerPoll.dumpState();

        std::cerr << "Document Broker polls "
                  << "[ " << DocBrokers.size() << " ]:\n";
        for (auto &i : DocBrokers)
            i.second->dumpState();
    }

private:
    std::atomic<bool> _stop;

    class AcceptPoll : public TerminatingPoll {
    public:
        AcceptPoll(const std::string &threadName) :
            TerminatingPoll(threadName) {}

        void wakeupHook() override
        {
            if (DumpGlobalState)
            {
                dump_state();
                DumpGlobalState = false;
            }
        }
    };
    /// This thread & poll accepts incoming connections.
    AcceptPoll _acceptPoll;

    /// Create a new server socket - accepted sockets will be added
    /// to the @clientSockets' poll when created with @factory.
    std::shared_ptr<ServerSocket> getServerSocket(const Poco::Net::SocketAddress& addr,
                                                  SocketPoll &clientSocket,
                                                  std::shared_ptr<SocketFactory> factory)
    {
        std::shared_ptr<ServerSocket> serverSocket = std::make_shared<ServerSocket>(clientSocket, factory);

        if (!serverSocket->bind(addr))
        {
            LOG_ERR("Failed to bind to: " << addr.toString());
            return nullptr;
        }

        if (serverSocket->listen())
            return serverSocket;

        LOG_ERR("Failed to listen on: " << addr.toString());
        return nullptr;
    }

    std::shared_ptr<ServerSocket> findPrisonerServerPort(int port)
    {
        std::shared_ptr<SocketFactory> factory = std::make_shared<PrisonerSocketFactory>();
        std::shared_ptr<ServerSocket> socket = getServerSocket(SocketAddress("127.0.0.1", port),
                                                               PrisonerPoll, factory);

        if (!UnitWSD::isUnitTesting() && !socket)
        {
            LOG_FTL("Failed to listen on Prisoner master port (" <<
                    MasterPortNumber << "). Exiting.");
            _exit(Application::EXIT_SOFTWARE);
        }

        while (!socket)
        {
            ++port;
            LOG_INF("Prisoner port " << (port - 1) << " is busy, trying " << port << ".");
            socket = getServerSocket(SocketAddress("127.0.0.1", port),
                                     PrisonerPoll, factory);
        }

        return socket;
    }

    std::shared_ptr<ServerSocket> findServerPort(int port)
    {
        LOG_INF("Trying to listen on client port " << port << ".");
        std::shared_ptr<SocketFactory> factory;
#if ENABLE_SSL
        if (LOOLWSD::isSSLEnabled())
            factory = std::make_shared<SslSocketFactory>();
        else
#endif
            factory = std::make_shared<PlainSocketFactory>();

        std::shared_ptr<ServerSocket> socket = getServerSocket(SocketAddress(port),
                                                               WebServerPoll, factory);
        while (!socket)
        {
            ++port;
            LOG_INF("Client port " << (port - 1) << " is busy, trying " << port << ".");
            socket = getServerSocket(SocketAddress(port),
                                     WebServerPoll, factory);
        }

        LOG_INF("Listening to client connections on port " << port);
        return socket;
    }
};

LOOLWSDServer srv;

bool LOOLWSD::handleShutdownRequest()
{
    if (ShutdownRequestFlag)
    {
        LOG_INF("Shutdown requested. Initiating WSD shutdown.");
        Util::alertAllUsers("close: shuttingdown");
        ShutdownFlag = true;
        return true;
    }

    return false;
}

int LOOLWSD::main(const std::vector<std::string>& /*args*/)
{
#ifndef FUZZER
    SigUtil::setUserSignals();
    SigUtil::setFatalSignals();
    SigUtil::setTerminationSignals();
#endif

    // down-pay all the forkit linking cost once & early.
    Environment::set("LD_BIND_NOW", "1");

    if (DisplayVersion)
    {
        std::string version, hash;
        Util::getVersionInfo(version, hash);
        LOG_INF("Loolwsd version details: " << version << " - " << hash);
    }

    initializeSSL();

    char* locale = setlocale(LC_ALL, nullptr);
    if (locale == nullptr || std::strcmp(locale, "C") == 0)
    {
        setlocale(LC_ALL, "en_US.utf8");
    }

    if (access(Cache.c_str(), R_OK | W_OK | X_OK) != 0)
    {
        LOG_SFL("Unable to access cache [" << Cache <<
                "] please make sure it exists, and has write permission for this user.");
        return Application::EXIT_SOFTWARE;
    }

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (SysTemplate.empty())
    {
        LOG_FTL("Missing --systemplate option");
        throw MissingOptionException("systemplate");
    }
    if (LoTemplate.empty())
    {
        LOG_FTL("Missing --lotemplate option");
        throw MissingOptionException("lotemplate");
    }
    if (ChildRoot.empty())
    {
        LOG_FTL("Missing --childroot option");
        throw MissingOptionException("childroot");
    }
    else if (ChildRoot[ChildRoot.size() - 1] != '/')
        ChildRoot += '/';

    FileUtil::registerFileSystemForDiskSpaceChecks(ChildRoot);
    FileUtil::registerFileSystemForDiskSpaceChecks(Cache + "/.");

    if (FileServerRoot.empty())
        FileServerRoot = Poco::Path(Application::instance().commandPath()).parent().toString();
    FileServerRoot = Poco::Path(FileServerRoot).absolute().toString();
    LOG_DBG("FileServerRoot: " << FileServerRoot);

    if (ClientPortNumber == MasterPortNumber)
        throw IncompatibleOptionsException("port");

    // Start the internal prisoner server and spawn forkit,
    // which in turn forks first child.
    srv.startPrisoners(MasterPortNumber);

#ifndef KIT_IN_PROCESS
    {
        std::unique_lock<std::mutex> lock(NewChildrenMutex);

        const auto timeoutMs = CHILD_TIMEOUT_MS * (LOOLWSD::NoCapsForKit ? 150 : 3);
        const auto timeout = std::chrono::milliseconds(timeoutMs);
        // Make sure we have at least one before moving forward.
        LOG_TRC("Waiting for a new child for a max of " << timeoutMs << " ms.");
        if (!NewChildrenCV.wait_for(lock, timeout, []() { return !NewChildren.empty(); }))
        {
            const auto msg = "Failed to fork child processes.";
            LOG_FTL(msg);
            throw std::runtime_error(msg);
        }

        // Check we have at least one.
        LOG_TRC("Have " << NewChildren.size() << " new children.");
        assert(NewChildren.size() > 0);
    }
#endif

    // Start the server.
    srv.start(ClientPortNumber);

#if ENABLE_DEBUG
    time_t startTimeSpan = time(nullptr);
#endif

    /// Something of a hack to get woken up on exit.

    SocketPoll mainWait("main");
    while (!TerminationFlag && !ShutdownRequestFlag)
    {
        UnitWSD::get().invokeTest();

        mainWait.poll(SocketPoll::DefaultPollTimeoutMs * 10);

        std::unique_lock<std::mutex> docBrokersLock(DocBrokersMutex);
        cleanupDocBrokers();

#if ENABLE_DEBUG
        if (careerSpanSeconds > 0 && time(nullptr) > startTimeSpan + careerSpanSeconds)
        {
            LOG_INF((time(nullptr) - startTimeSpan) << " seconds gone, finishing as requested.");
            break;
        }
#endif
    }

    // Stop the listening to new connections
    // and wait until sockets close.
    LOG_INF("Stopping server socket listening. ShutdownFlag: " <<
            ShutdownRequestFlag << ", TerminationFlag: " << TerminationFlag);

    // Wait until documents are saved and sessions closed.
    srv.stop();
    WebServerPoll.stop();

    // atexit handlers tend to free Admin before Documents
    LOG_INF("Cleaning up lingering documents.");
    DocBrokers.clear();

#ifndef KIT_IN_PROCESS
    // Terminate child processes
    LOG_INF("Requesting forkit process " << ForKitProcId << " to terminate.");
    SigUtil::killChild(ForKitProcId);
#endif

    // Terminate child processes
    LOG_INF("Requesting child processes to terminate.");
    for (auto& child : NewChildren)
    {
        child->close(true);
    }

#ifndef KIT_IN_PROCESS
    // Wait for forkit process finish.
    int status = 0;
    waitpid(ForKitProcId, &status, WUNTRACED);
    close(ForKitWritePipe);
#endif

    // In case forkit didn't cleanup properly, don't leave jails behind.
    LOG_INF("Cleaning up childroot directory [" << ChildRoot << "].");
    std::vector<std::string> jails;
    File(ChildRoot).list(jails);
    for (auto& jail : jails)
    {
        const auto path = ChildRoot + jail;
        LOG_INF("Removing jail [" << path << "].");
        FileUtil::removeFile(path, true);
    }

    if (isShuttingDown())
    {
#if 0 // loolnb
        // At this point there should be no other thread, but...
        std::lock_guard<std::mutex> lock(ClientWebSocketsMutex);

        LOG_INF("Notifying clients that we are recycling.");
        static const std::string msg("close: recycling");
        for (auto& ws : ClientWebSockets)
        {
            try
            {
                ws->sendFrame(msg.data(), msg.size());
                ws->shutdown(WebSocket::WS_ENDPOINT_GOING_AWAY);
            }
            catch (const std::exception& ex)
            {
                LOG_ERR("Error while notifying client of recycle: " << ex.what());
            }
        }
#endif
    }

    // Finally, we no longer need SSL.
    if (LOOLWSD::isSSLEnabled())
    {
        Poco::Net::uninitializeSSL();
        Poco::Crypto::uninitializeCrypto();
#if ENABLE_SSL
        SslContext::uninitialize();
#endif
    }

    int returnValue = Application::EXIT_OK;
    UnitWSD::get().returnValue(returnValue);

    LOG_INF("Process [loolwsd] finished.");
    return returnValue;
}

void UnitWSD::testHandleRequest(TestRequest type, UnitHTTPServerRequest& /* request */, UnitHTTPServerResponse& /* response */)
{
    switch (type)
    {
    case TestRequest::Client:
#if 0 // loolnb
        ClientRequestHandler::handleClientRequest(request, response, LOOLWSD::GenSessionId());
        break;
    case TestRequest::Prisoner:
        PrisonerRequestHandler::handlePrisonerRequest(request, response);
#endif
        break;
    default:
        assert(false);
        break;
    }
}

#if !defined(BUILDING_TESTS) && !defined(KIT_IN_PROCESS)
namespace Util
{

void alertAllUsers(const std::string& cmd, const std::string& kind)
{
    alertAllUsers("error: cmd=" + cmd + " kind=" + kind);
}

void alertAllUsers(const std::string& msg)
{
    std::lock_guard<std::mutex> docBrokersLock(DocBrokersMutex);

    alertAllUsersInternal(msg);
}

}
#endif

void dump_state()
{
    srv.dumpState();
}

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
