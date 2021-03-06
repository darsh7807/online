/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "config.h"

#include "net/DelaySocket.hpp"

#define DELAY_LOG(X) std::cerr << X << "\n";

class Delayer;

// FIXME: TerminatingPoll ?
static SocketPoll DelayPoll("delay_poll");

/// Reads from fd, delays that and then writes to _dest.
class DelaySocket : public Socket {
    int _delayMs;
    enum State { ReadWrite,      // normal socket
                 EofFlushWrites, // finish up writes and close
                 Closed };
    State _state;
    std::shared_ptr<DelaySocket> _dest; // our writing twin.

    const size_t WindowSize = 64 * 1024;

    /// queued up data - sent to us by our opposite twin.
    struct WriteChunk {
        std::chrono::steady_clock::time_point _sendTime;
        std::vector<char> _data;
        WriteChunk(int delayMs)
        {
            _sendTime = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(delayMs);
        }
        bool isError() { return _data.size() == 0; }
    private:
        WriteChunk();
    };

    std::vector<std::shared_ptr<WriteChunk>> _chunks;
public:
    DelaySocket(int delayMs, int fd) :
        Socket (fd), _delayMs(delayMs),
        _state(ReadWrite)
	{
//        setSocketBufferSize(Socket::DefaultSendBufferSize);
	}
    void setDestination(const std::shared_ptr<DelaySocket> &dest)
    {
        _dest = dest;
    }

    void dumpState(std::ostream& os) override
    {
        os << "\tfd: " << getFD()
           << "\n\tqueue: " << _chunks.size() << "\n";
        auto now = std::chrono::steady_clock::now();
        for (auto &chunk : _chunks)
        {
            os << "\t\tin: " <<
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    chunk->_sendTime - now).count() << "ms - "
               << chunk->_data.size() << "bytes\n";
        }
    }

    // FIXME - really need to propagate 'noDelay' etc.
    // have a debug only lookup of delayed sockets for this case ?

    int getPollEvents(std::chrono::steady_clock::time_point now,
                      int &timeoutMaxMs) override
    {
        if (_chunks.size() > 0)
        {
            int remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                (*_chunks.begin())->_sendTime - now).count();
            if (remainingMs < timeoutMaxMs)
                DELAY_LOG("#" << getFD() << " reset timeout max to " << remainingMs
                          << "ms from " << timeoutMaxMs << "ms\n");
            timeoutMaxMs = std::min(timeoutMaxMs, remainingMs);
        }

        if (_chunks.size() > 0 &&
            now > (*_chunks.begin())->_sendTime)
            return POLLIN | POLLOUT;
        else
            return POLLIN;
    }

    void pushCloseChunk()
    {
        _chunks.push_back(std::make_shared<WriteChunk>(_delayMs));
    }

    void changeState(State newState)
    {
        switch (newState)
        {
        case ReadWrite:
            assert (false);
            break;
        case EofFlushWrites:
            assert (_state == ReadWrite);
            assert (_dest);
            _dest->pushCloseChunk();
            _dest = nullptr;
            break;
        case Closed:
            if (_dest && _state == ReadWrite)
                _dest->pushCloseChunk();
            _dest = nullptr;
            shutdown();
            break;
        }
        DELAY_LOG("#" << getFD() << " changed to state " << newState << "\n");
        _state = newState;
    }

    void handlePoll(SocketDisposition &disposition,
                    std::chrono::steady_clock::time_point now, int events) override
    {
        if (_state == ReadWrite && (events & POLLIN))
        {
            auto chunk = std::make_shared<WriteChunk>(_delayMs);

            char buf[64 * 1024];
            ssize_t len;
            size_t toRead = sizeof(buf); //std::min(sizeof(buf), WindowSize - _chunksSize);
            do {
                len = ::read(getFD(), buf, toRead);
            } while (len < 0 && errno == EINTR);

            if (len == 0) // EOF.
                changeState(EofFlushWrites);
            else if (len >= 0)
            {
                DELAY_LOG("#" << getFD() << " read " << len
                          << " to queue: " << _chunks.size() << "\n");
                chunk->_data.insert(chunk->_data.end(), &buf[0], &buf[len]);
                if (_dest)
                    _dest->_chunks.push_back(chunk);
                else
                    assert("no destination for data" && false);
            }
            else if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                DELAY_LOG("#" << getFD() << " error : " << errno << " " << strerror(errno) << "\n");
                changeState(Closed); // FIXME - propagate the error ?
            }
        }

        if (_chunks.size() == 0)
        {
            if (_state == EofFlushWrites)
                changeState(Closed);
        }
        else // Write if we have delayed enough.
        {
            std::shared_ptr<WriteChunk> chunk = *_chunks.begin();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - chunk->_sendTime).count() >= 0)
            {
                if (chunk->_data.size() == 0)
                { // delayed error or close
                    DELAY_LOG("#" << getFD() << " handling delayed close\n");
                    changeState(Closed);
                }
                else
                {
                    ssize_t len;
                    do {
                        len = ::write(getFD(), &chunk->_data[0], chunk->_data.size());
                    } while (len < 0 && errno == EINTR);

                    if (len < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            DELAY_LOG("#" << getFD() << " full - waiting for write\n");
                        }
                        else
                        {
                            DELAY_LOG("#" << getFD() << " failed onwards write "
                                      << len << "bytes of "
                                      << chunk->_data.size()
                                      << " queue: " << _chunks.size() << " error "
                                      << strerror(errno) << "\n");
                            changeState(Closed);
                        }
                    }
                    else
                    {
                        DELAY_LOG("#" << getFD() << " written onwards " << len << "bytes of "
                                  << chunk->_data.size()
                                  << " queue: " << _chunks.size() << "\n");
                        if (len > 0)
                            chunk->_data.erase(chunk->_data.begin(), chunk->_data.begin() + len);

                        if (chunk->_data.size() == 0)
                            _chunks.erase(_chunks.begin(), _chunks.begin() + 1);
                    }
                }
            }
        }

        if (events & (POLLERR | POLLHUP | POLLNVAL))
        {
            DELAY_LOG("#" << getFD() << " error events: " << events << "\n");
            changeState(Closed);
        }

        if (_state == Closed)
            disposition.setClosed();
    }
};

/// Delayer:
///
/// Some terminology:
///    physical socket (DelaySocket's own fd) - what we accepted.
///    internalFd - the internal side of the socket-pair
///    delayFd - what we hand on to our un-suspecting wrapped socket
///              which looks like an external socket - but delayed.
namespace Delay {
    int create(int delayMs, int physicalFd)
    {
        int pair[2];
        int rc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair);
        assert (rc == 0); (void)rc;
        int internalFd = pair[0];
        int delayFd = pair[1];

        auto physical = std::make_shared<DelaySocket>(delayMs, physicalFd);
        auto internal = std::make_shared<DelaySocket>(delayMs, internalFd);
        physical->setDestination(internal);
        internal->setDestination(physical);

        DelayPoll.startThread();
        DelayPoll.insertNewSocket(physical);
        DelayPoll.insertNewSocket(internal);

        return delayFd;
    }
    void dumpState(std::ostream &os)
    {
        if (DelayPoll.isAlive())
        {
            os << "Delay poll:\n";
            DelayPoll.dumpState(os);
        }
    }
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
