/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Parts of this file is covered by:

Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

 */

#include "config.h"

#include <unistd.h>

#include <cstdlib>
#include <iostream>

#define LOK_USE_UNSTABLE_API
#include <LibreOfficeKit/LibreOfficeKitInit.h>

#include <Poco/Format.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/WebSocket.h>
#include <Poco/Process.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/Option.h>
#include <Poco/Util/OptionException.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

#include "LOOLSession.hpp"
#include "LOOLWSD.hpp"
#include "Util.hpp"

using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::ServerSocket;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Runnable;
using Poco::Thread;
using Poco::Util::Application;
using Poco::Util::HelpFormatter;
using Poco::Util::IncompatibleOptionsException;
using Poco::Util::MissingOptionException;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::ServerApplication;

class WebSocketRequestHandler: public HTTPRequestHandler
    /// Handle a WebSocket connection.
{
public:
    WebSocketRequestHandler()
    {
    }

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override
    {
        if(!(request.find("Upgrade") != request.end() && Poco::icompare(request["Upgrade"], "websocket") == 0))
        {
            response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
            response.setContentLength(0);
            response.send();
            return;
        }

        Application& app = Application::instance();
        try
        {
            WebSocket ws(request, response);

            LOOLSession session(ws);

            // Loop, receiving WebSocket messages either from the
            // client, or from the child process (to be forwarded to
            // the client).
            int flags;
            int n;
            ws.setReceiveTimeout(0);
            do
            {
                char buffer[100000];
                n = ws.receiveFrame(buffer, sizeof(buffer), flags);

                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                    if (!session.handleInput(buffer, n))
                        n = 0;
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        }
        catch (WebSocketException& exc)
        {
            app.logger().error(Util::logPrefix() + "WebSocketException: " + exc.message());
            switch (exc.code())
            {
            case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
                response.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
                // fallthrough
            case WebSocket::WS_ERR_NO_HANDSHAKE:
            case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
            case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
                response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
                response.setContentLength(0);
                response.send();
                break;
            }
        }
    }
};

class RequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
    RequestHandlerFactory()
    {
    }

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request) override
    {
        Application& app = Application::instance();
        std::string line = (Util::logPrefix() + "Request from " +
                            request.clientAddress().toString() + ": " +
                            request.getMethod() + " " +
                            request.getURI() + " " +
                            request.getVersion());

        for (HTTPServerRequest::ConstIterator it = request.begin(); it != request.end(); ++it)
        {
            line += " / " + it->first + ": " + it->second;
        }

        app.logger().information(line);
        return new WebSocketRequestHandler();
    }
};

class TestOutput: public Runnable
{
public:
    TestOutput(WebSocket& ws) :
        _ws(ws)
    {
    }

    void run() override
    {
        int flags;
        int n;
        Application& app = Application::instance();
        _ws.setReceiveTimeout(0);
        try
        {
            do
            {
                char buffer[100000];
                n = _ws.receiveFrame(buffer, sizeof(buffer), flags);

                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                {
                    char *endl = (char *) memchr(buffer, '\n', n);
                    std::string response;
                    if (endl == nullptr)
                        response = std::string(buffer, n);
                    else
                        response = std::string(buffer, endl-buffer);
                    std::cout <<
                        Util::logPrefix() << "Client got " << n << " bytes: '" << response << "'" <<
                        (endl == nullptr ? "" : " ...") <<
                        std::endl;
                }
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
        }
        catch (WebSocketException& exc)
        {
            app.logger().error(Util::logPrefix() + "WebSocketException: " + exc.message());
            _ws.close();
        }
    }

private:
    WebSocket& _ws;
};

class TestInput: public Runnable
{
public:
    TestInput(ServerApplication& main, ServerSocket& svs, HTTPServer& srv) :
        _main(main),
        _svs(svs),
        _srv(srv)
    {
    }

    void run() override
    {
        HTTPClientSession cs("localhost", _svs.address().port());
        HTTPRequest request(HTTPRequest::HTTP_GET, "/ws");
        HTTPResponse response;
        WebSocket ws(cs, request, response);

        Thread thread;
        TestOutput output(ws);
        thread.start(output);

        if (isatty(0))
        {
            std::cout << std::endl;
            std::cout << "Enter LOOL WS requests, one per line. Enter EOF to finish." << std::endl;
        }

        while (!std::cin.eof())
        {
            std::string line;
            std::getline(std::cin, line);
            ws.sendFrame(line.c_str(), line.size());
        }
        thread.join();
        _srv.stopAll();
        _main.terminate();
    }

private:
    ServerApplication& _main;
    ServerSocket& _svs;
    HTTPServer& _srv;
};

int LOOLWSD::portNumber = DEFAULT_PORT_NUMBER;
std::string LOOLWSD::sysTemplate;
std::string LOOLWSD::loTemplate;
std::string LOOLWSD::childRoot;
std::string LOOLWSD::loSubPath = "lo";
std::string LOOLWSD::jail;
int LOOLWSD::numPreForkedChildren = 10;

LOOLWSD::LOOLWSD() :
    _doTest(false),
    _childId(0)
{
}

LOOLWSD::~LOOLWSD()
{
}

void LOOLWSD::initialize(Application& self)
{
    ServerApplication::initialize(self);
}

void LOOLWSD::uninitialize()
{
    ServerApplication::uninitialize();
}

void LOOLWSD::defineOptions(OptionSet& options)
{
    ServerApplication::defineOptions(options);

    options.addOption(Option("help", "", "display help information on command line arguments")
                      .required(false)
                      .repeatable(false));

    options.addOption(Option("port", "", "port number to listen to (default: " + std::to_string(LOOLWSD::DEFAULT_PORT_NUMBER) + ")")
                      .required(false)
                      .repeatable(false)
                      .argument("port number"));

    options.addOption(Option("systemplate", "", "path to a template tree with shared libraries etc to be used as source for chroot jails for child processes")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("lotemplate", "", "path to a LibreOffice installation tree to be copied (linked) into the jails for child processes")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("childroot", "", "path to the directory under which the chroot jails for the child processes will be created")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));

    options.addOption(Option("losubpath", "", "relative path where the LibreOffice installation will be copied inside a jail (default: '" + loSubPath + "')")
                      .required(false)
                      .repeatable(false)
                      .argument("relative path"));

    options.addOption(Option("numpreforks", "", "number of child processes to keep waiting for new clients")
                      .required(false)
                      .repeatable(false)
                      .argument("port number"));

    options.addOption(Option("test", "", "interactive testing")
                      .required(false)
                      .repeatable(false));

    options.addOption(Option("child", "", "for internal use only")
                      .required(false)
                      .repeatable(false)
                      .argument("child id"));

    options.addOption(Option("jail", "", "for internal use only")
                      .required(false)
                      .repeatable(false)
                      .argument("directory"));
}

void LOOLWSD::handleOption(const std::string& name, const std::string& value)
{
    ServerApplication::handleOption(name, value);

    if (name == "help")
    {
        displayHelp();
        exit(Application::EXIT_OK);
    }
    else if (name == "port")
        portNumber = std::stoi(value);
    else if (name == "systemplate")
        sysTemplate = value;
    else if (name == "lotemplate")
        loTemplate = value;
    else if (name == "childroot")
        childRoot = value;
    else if (name == "losubpath")
        loSubPath = value;
    else if (name == "numpreforks")
        numPreForkedChildren = std::stoi(value);
    else if (name == "test")
        _doTest = true;
    else if (name == "child")
        _childId = std::stoull(value);
    else if (name == "jail")
        jail = value;
}

void LOOLWSD::displayHelp()
{
    HelpFormatter helpFormatter(options());
    helpFormatter.setCommand(commandName());
    helpFormatter.setUsage("OPTIONS");
    helpFormatter.setHeader("LibreOffice On-Line WebSocket server.");
    helpFormatter.format(std::cout);
}

int LOOLWSD::childMain()
{
    std::cout << Util::logPrefix() << "Child here!" << std::endl;

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the child (but not in the
    // parent) separately now. And also for options that are
    // meaningless to the child.
    if (jail == "")
        throw MissingOptionException("systemplate");

    if (sysTemplate != "")
        throw IncompatibleOptionsException("systemplate");
    if (loTemplate != "")
        throw IncompatibleOptionsException("lotemplate");
    if (childRoot != "")
        throw IncompatibleOptionsException("childroot");

    if (chroot(jail.c_str()) == -1)
    {
        logger().error("chroot(\"" + jail + "\") failed: " + strerror(errno));
        exit(1);
    }

    if (chdir("/") == -1)
    {
        logger().error(std::string("chdir(\"/\") in jail failed: ") + strerror(errno));
        exit(1);
    }

    if (std::getenv("SLEEPFORDEBUGGER"))
    {
        std::cout << "Sleeping " << std::getenv("SLEEPFORDEBUGGER") << " seconds, " <<
            "attach process " << Poco::Process::id() << " in debugger now." << std::endl;
        Thread::sleep(std::stoul(std::getenv("SLEEPFORDEBUGGER")) * 1000);
    }

    LibreOfficeKit *loKit(lok_init_2(("/" + loSubPath + "/program").c_str(), "file:///user"));

    if (!loKit)
    {
        logger().fatal(Util::logPrefix() + "LibreOfficeKit initialisation failed");
        return Application::EXIT_UNAVAILABLE;
    }

    // Open websocket connection between the child process and the
    // parent. The parent forwars us requests that it can't handle.

    HTTPClientSession cs("localhost", portNumber);
    HTTPRequest request(HTTPRequest::HTTP_GET, "/ws");
    HTTPResponse response;
    WebSocket ws(cs, request, response);

    LOOLSession session(ws, loKit);

    ws.setReceiveTimeout(0);

    std::string hello("child " + std::to_string(_childId));
    session.sendTextFrame(hello);

    int flags;
    int n;
    do
    {
        char buffer[1024];
        n = ws.receiveFrame(buffer, sizeof(buffer), flags);

        if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
            if (!session.handleInput(buffer, n))
                n = 0;
    }
    while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);

    return Application::EXIT_OK;
}

int LOOLWSD::main(const std::vector<std::string>& args)
{
    if (access(LOOLWSD_CACHEDIR, R_OK | W_OK | X_OK) != 0)
    {
        std::cout << "Unable to access " << LOOLWSD_CACHEDIR <<
            ", please make sure it exists, and has write permission for this user." << std::endl;
        return Application::EXIT_UNAVAILABLE;
    }

    if (childMode())
        return childMain();

    // We use the same option set for both parent and child loolwsd,
    // so must check options required in the parent (but not in the
    // child) separately now. Also check for options that are
    // meaningless for the parent.
    if (sysTemplate == "")
        throw MissingOptionException("systemplate");
    if (loTemplate == "")
        throw MissingOptionException("lotemplate");
    if (childRoot == "")
        throw MissingOptionException("childroot");

    if (_childId != 0)
        throw IncompatibleOptionsException("child");
    if (jail != "")
        throw IncompatibleOptionsException("jail");

    for (int i = 0; i < numPreForkedChildren; i++)
        LOOLSession::preFork();

    ServerSocket svs(portNumber);

    HTTPServer srv(new RequestHandlerFactory(), svs, new HTTPServerParams);

    srv.start();

    Thread thread;
    TestInput input(*this, svs, srv);
    if (_doTest)
    {
        thread.start(input);
    }

    waitForTerminationRequest();

    srv.stop();

    if (_doTest)
        thread.join();

    return Application::EXIT_OK;
}

bool LOOLWSD::childMode() const
{
    return _childId != 0;
}

POCO_SERVER_MAIN(LOOLWSD)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
