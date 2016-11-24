/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/PrivateKeyPassphraseHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/SocketStream.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/TCPServer.h>
#include <Poco/Net/TCPServerConnection.h>
#include <Poco/Net/TCPServerConnectionFactory.h>
#include <Poco/Process.h>
#include <Poco/SharedPtr.h>
#include <Poco/StringTokenizer.h>
#include <Poco/TemporaryFile.h>
#include <Poco/Thread.h>
#include <Poco/URI.h>
#include <Poco/Util/Application.h>
#include <Poco/Version.h>

#include "Common.hpp"
#include "LOOLProtocol.hpp"
#include <LOOLWebSocket.hpp>
#include "Log.hpp"
#include "Util.hpp"

using namespace LOOLProtocol;

using Poco::Net::AcceptCertificateHandler;
using Poco::Net::Context;
#if ENABLE_SSL
using Poco::Net::HTTPSClientSession;
#else
using Poco::Net::HTTPClientSession;
#endif
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPResponse;
using Poco::Net::InvalidCertificateHandler;
using Poco::Net::SSLManager;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Runnable;
using Poco::SharedPtr;
using Poco::StringTokenizer;
using Poco::TemporaryFile;
using Poco::Thread;
using Poco::URI;
using Poco::Util::Application;

static bool closeExpected = false;
static std::mutex coutMutex;

/// Prints incoming data from a LOOLWebSocket.
class Output : public Runnable
{
public:
    Output(LOOLWebSocket& ws) :
        _ws(ws)
    {
    }

    void run() override
    {
        int flags;
        int n;
        try
        {
            do
            {
                char buffer[100000];
                n = _ws.receiveFrame(buffer, sizeof(buffer), flags);
                if (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE)
                {
                    {
                        std::unique_lock<std::mutex> lock(coutMutex);
                        std::cout << "Got " << getAbbreviatedFrameDump(buffer, n, flags) << std::endl;
                    }

                    std::string firstLine = getFirstLine(buffer, n);
                    StringTokenizer tokens(firstLine, " ", StringTokenizer::TOK_IGNORE_EMPTY | StringTokenizer::TOK_TRIM);

                    if (std::getenv("DISPLAY") != nullptr && tokens[0] == "tile:")
                    {
                        TemporaryFile pngFile;
                        std::ofstream pngStream(pngFile.path(), std::ios::binary);
                        pngStream.write(buffer + firstLine.size() + 1, n - firstLine.size() - 1);
                        pngStream.close();
                        if (std::system((std::string("display ") + pngFile.path()).c_str()) == -1)
                        {
                            // Not worth it to display a warning, this is just a throwaway test program, and
                            // the developer running it surely notices if nothing shows up...
                        }
                    }
                }
            }
            while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);

            {
                std::unique_lock<std::mutex> lock(coutMutex);
                std::cout << "CLOSE frame received" << std::endl;
            }
            if (!closeExpected)
                std::_Exit(Application::EXIT_SOFTWARE);
        }
        catch (WebSocketException& exc)
        {
            std::unique_lock<std::mutex> lock(coutMutex);
            std::cout << "Got exception " << exc.message() << std::endl;
        }
    }

    LOOLWebSocket& _ws;
};

/// Program for interactive or scripted testing of a lool server.
class Connect: public Poco::Util::Application
{
public:
    Connect() :
#if ENABLE_SSL
        _uri("https://127.0.0.1:" + std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "/ws")
#else
        _uri("http://127.0.0.1:" + std::to_string(DEFAULT_CLIENT_PORT_NUMBER) + "/ws")
#endif
    {
    }

    ~Connect()
    {
    }

protected:
    int main(const std::vector<std::string>& args) override
    {
        if (args.size() < 1)
        {
            Log::error("Usage: connect documentURI [serverURI]");
            return Application::EXIT_USAGE;
        }

        if (args.size() > 1)
            _uri = URI(args[1]);

#if ENABLE_SSL
        Poco::Net::initializeSSL();

        SharedPtr<InvalidCertificateHandler> invalidCertHandler = new AcceptCertificateHandler(false);
        Context::Params sslParams;
        Context::Ptr sslContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslParams);
        SSLManager::instance().initializeClient(0, invalidCertHandler, sslContext);

        HTTPSClientSession cs(_uri.getHost(), _uri.getPort());
#else
        HTTPClientSession cs(_uri.getHost(), _uri.getPort());
#endif
        std::string encodedUri;
        URI::encode(args[0], ":/?", encodedUri);
        HTTPRequest request(HTTPRequest::HTTP_GET, "/lool/" + encodedUri + "/ws");
        HTTPResponse response;
        LOOLWebSocket ws(cs, request, response);

        ws.setReceiveTimeout(0);

        Thread thread;
        Output output(ws);
        thread.start(output);

        while (true)
        {
            std::string line;
            std::getline(std::cin, line);
            if (std::cin.eof())
            {
                break;
            }
            else if (line.find("sleep ") == 0)
            {
                // Accept an input line "sleep <n>" that makes us sleep a number of seconds.
                long sleepTime = std::stol(line.substr(std::string("sleep").length()));
                {
                    std::unique_lock<std::mutex> lock(coutMutex);
                    std::cout << "Sleeping " << sleepTime << " seconds" << std::endl;
                }

                std::this_thread::sleep_for(std::chrono::seconds(sleepTime));
            }
            else if (line == "exit")
            {
                // While hacking on LOOL and editing input files for this program back and forth it
                // is a good idea to be able to add an enforced exit in the middle of the input
                // file.
                {
                    std::unique_lock<std::mutex> lock(coutMutex);
                    std::cout << "Exiting" << std::endl;
                }
                break;
            }
            else if (line.find('#') == 0)
            {
                // Also comments can be useful in input files for this program
            }
            else
            {
                {
                    std::unique_lock<std::mutex> lock(coutMutex);
                    std::cout << "Sending: '" << line << "'" << std::endl;
                }
                ws.sendFrame(line.c_str(), line.size());
            }
        }

        {
            std::unique_lock<std::mutex> lock(coutMutex);
            std::cout << "Shutting down websocket" << std::endl;
        }
        closeExpected = true;
        ws.shutdown();
        thread.join();

        return Application::EXIT_OK;
    }

private:
    URI _uri;
};

namespace Util
{

void alertAllUsers(const std::string& cmd, const std::string& kind)
{
    std::cout << "error: cmd=" << cmd << " kind=" << kind << std::endl;
    (void) kind;
}

}

POCO_APP_MAIN(Connect)

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */