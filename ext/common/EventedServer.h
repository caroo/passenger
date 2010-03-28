/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2010 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_EVENTED_SERVER_H_
#define _PASSENGER_EVENTED_SERVER_H_

#include <ev++.h>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <oxt/system_calls.hpp>
#include <oxt/macros.hpp>
#include <algorithm>
#include <string>
#include <set>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cerrno>
#include "FileDescriptor.h"
#include "StaticString.h"
#include "Logging.h"
#include "Utils.h"

namespace Passenger {

using namespace std;
using namespace boost;
using namespace oxt;


/**
 * A base class for writing single-threaded, evented servers that use non-blocking I/O.
 * It uses libev for its event loop. EventedServer handles much of the situps regarding
 * client connection management and output buffering and tries to make it easy to
 * implement a zero-copy architecture.
 *
 * <h2>Basic usage</h2>
 * Derived classes can override the onClientReadable() method, which is called every time
 * a specific becomes readable. It is passed a Client object which contains information
 * about the client, such as its file descriptor. One can use the read() system call in
 * that method to receive data from the client. Please note that client file descriptors
 * are always set to non-blocking mode so you need to handle this gracefully.
 *
 * EventedServer provides the write() method for sending data to the client. This method
 * will attempt to send the data to the client immediately; if it fails with EAGAIN then
 * EventedServer will take care of scheduling the send at a later time when the client
 * is ready again to receive data.
 *
 * To disconnect the client, call disconnect(). The connection might not be actually
 * closed until all pending outgoing data have been sent out, but all the gory details
 * is taken care of for you.
 *
 * <h2>Keeping per-client information</h2>
 * If you need to keep per-client information then you can override the createClient()
 * method and make it return an object that's a subclass of EventedServer::Client. This
 * object is passed to onClientReadable(), so in there you can just cast the client object
 * to your subclass.
 */
class EventedServer {
protected:
	/** Contains various client information. */
	struct Client: public enable_shared_from_this<Client> {
		enum {
			/**
			 * This is the initial state for a client. It means we're
			 * connected to the client, ready to receive data and
			 * there's no pending outgoing data.
			 */
			ES_CONNECTED,
			
			/**
			 * This state is entered from ES_CONNECTED when the write()
			 * method fails and EventedServer schedules data to be sent
			 * later. In here we might be watching for read events, but
			 * not if there's too much pending data, in which case
			 * EventedServer will concentrate on sending out all
			 * pending data before watching read events again.
			 */
			ES_WRITES_PENDING,
			
			/**
			 * This state is entered from ES_WRITES_PENDING state when
			 * disconnect() is called. It means that we want to close
			 * the connection as soon as all pending outgoing data has
			 * been sent. As soon as that happens it'll transition
			 * to ES_DISCONNECTED.
			 */
			ES_DISCONNECTING_WITH_WRITES_PENDING,
			
			/**
			 * Final state. Client connection has been closed. No
			 * I/O with the client is possible.
			 */
			ES_DISCONNECTED
		} state;
		
		EventedServer *server;
		FileDescriptor fd;
		ev::io readWatcher;
		ev::io writeWatcher;
		bool readWatcherStarted;
		bool writeWatcherStarted;
		/** Pending outgoing data is stored here. */
		string outbox;
		
		Client(EventedServer *_server)
			: server(_server),
			  readWatcher(_server->loop),
			  writeWatcher(_server->loop)
		{ }
		
		void _onReadable(ev::io &w, int revents) {
			server->onClientReadable(shared_from_this());
		}
		
		void _onWritable(ev::io &w, int revents) {
			server->onClientWritable(shared_from_this());
		}
		
		/** Returns an identifier for this client. */
		string name() const {
			return toString(fd);
		}
	};
	
	typedef shared_ptr<Client> ClientPtr;
	
	void write(const ClientPtr &client, const char *data) {
		write(client, StaticString(data));
	}
	
	void write(const ClientPtr &client, const StaticString &data) {
		write(client, &data, 1);
	}
	
	/**
	 * Sends data to the given client. This method will try to send the data
	 * immediately (in which no intermediate copies of the data will be made),
	 * but if the client is not yet ready to receive data then the data will
	 * be buffered and scheduled for sending later.
	 *
	 * If an I/O error was encountered then the client connection will be closed.
	 *
	 * If the client connection has already been closed then this method
	 * does nothing.
	 */
	void write(const ClientPtr &client, const StaticString data[], size_t count) {
		if (client->state == Client::ES_DISCONNECTED) {
			return;
		}
		
		ssize_t ret;
		size_t totalSize;
		this_thread::disable_syscall_interruption dsi;
		
		if (client->outbox.empty()) {
			struct iovec iov[count];
			
			totalSize = staticStringArrayToIoVec(data, count, iov);
			ret = syscalls::writev(client->fd, iov, count);
			if (ret == -1) {
				if (errno == EAGAIN) {
					// Schedule all data for sending later.
					// TODO: reserve capacity
					for (size_t i = 0; i < count; i++) {
						client->outbox.append(data[i].data(), data[i].size());
					}
				} else {
					int e = errno;
					disconnect(client, true);
					logSystemError(client, "Cannot write data to client", e);
					return;
				}
			} else if ((size_t) ret < totalSize) {
				size_t index, offset;
				
				// Schedule unsent data for sending later.
				// TODO: reserve capacity
				findDataPositionIndexAndOffset(iov, count + 1, ret, &index, &offset);
				for (size_t i = index; i < count; i++) {
					if (i == index) {
						client->outbox.append(data[i].data() + offset,
							data[i].size() - offset);
					} else {
						client->outbox.append(data[i].data(),
							data[i].size());
					}
				}
			}
		} else {
			struct iovec iov[count + 1];
			
			iov[0].iov_base = (char *) client->outbox.data();
			iov[0].iov_len  = client->outbox.size();
			totalSize = staticStringArrayToIoVec(data, count, iov + 1);
			
			ret = syscalls::writev(client->fd, iov, count + 1);
			if (ret == -1) {
				if (errno != EAGAIN) {
					int e = errno;
					disconnect(client, true);
					logSystemError(client, "Cannot write data to client", e);
					return;
				}
				// else: send the outbox on the next writable event.
			} else {
				string::size_type outboxSize = client->outbox.size();
				size_t outboxSent = std::min((size_t) ret, outboxSize);
				
				// Remove everything in the outbox that we've been able to sent.
				client->outbox.erase(0, outboxSent);
				if (client->outbox.empty()) {
					size_t index, offset;
					
					// Looks like everything in the outbox was sent.
					// Schedule the rest of the data for sending later.
					// TODO: reserve capacity
					findDataPositionIndexAndOffset(iov, count + 1, ret, &index, &offset);
					for (size_t i = index; i < count + 1; i++) {
						if (i == index) {
							client->outbox.append(
								data[i - 1].data() + offset,
								data[i - 1].size() - offset);
						} else {
							client->outbox.append(
								data[i - 1].data(),
								data[i - 1].size());
						}
					}
				} else {
					// The outbox could only be partially sent out, so
					// nothing in 'data' could be sent. Add everything
					// in 'data' into the outbox.
					// TODO: reserve capacity
					for (size_t i = 1; i < count + 1; i++) {
						client->outbox.append(data[i - 1].data(),
							data[i - 1].size());
					}
				}
			}
		}
		if (client->outbox.empty()) {
			outboxFlushed(client);
		} else {
			outboxPartiallyFlushed(client);
		}
	}
	
	/**
	 * Disconnects the client. If <em>force</em> is true then the client will
	 * be disconnected immediately, and any pending outgoing data will be
	 * discarded. Otherwise the client will be disconnected after all pending
	 * outgoing data have been sent; in the mean time no new data can be
	 * received from the client.
	 */
	virtual void disconnect(const ClientPtr &client, bool force = false) {
		this_thread::disable_syscall_interruption dsi;
		
		if (client->state == Client::ES_CONNECTED
		 || (force && client->state != Client::ES_DISCONNECTED)) {
			watchReadEvents(client, false);
			watchWriteEvents(client, false);
			try {
				client->fd.close();
			} catch (const SystemException &e) {
				logSystemError(client, e.brief(), e.code());
			}
			client->state = Client::ES_DISCONNECTED;
			clients.erase(client);
		} else if (client->state == Client::ES_WRITES_PENDING) {
			watchReadEvents(client, false);
			watchWriteEvents(client, true);
			if (syscalls::shutdown(client->fd, SHUT_RD) == -1) {
				int e = errno;
				logSystemError(client,
					"Cannot shutdown reader half of the client socket",
					e);
			}
			client->state = Client::ES_DISCONNECTING_WITH_WRITES_PENDING;
		}
	}
	
	void logError(const ClientPtr &client, const string &message) {
		P_ERROR("Error in client " << client->name() << ": " << message);
	}
	
	void logSystemError(const ClientPtr &client, const string &message, int errorCode) {
		P_ERROR("Error in client " << client->name() << ": " <<
			message << ": " << strerror(errorCode) << " (" << errorCode << ")");
	}
	
	void logSystemError(const string &message, int errorCode) {
		P_ERROR(message << ": " << strerror(errorCode) << " (" << errorCode << ")");
	}
	
	virtual ClientPtr createClient() {
		return ClientPtr(new Client(this));
	}
	
	virtual void onNewClient(const ClientPtr &client) { }
	virtual void onClientReadable(const ClientPtr &client) { }

private:
	struct ev_loop *loop;
	FileDescriptor fd;
	ev::io acceptWatcher;
	set<ClientPtr> clients;
	
	/** Converts an array of StaticStrings to a corresponding array of iovec structures. */
	size_t staticStringArrayToIoVec(const StaticString ary[], size_t count, struct iovec *vec) {
		size_t total = 0;
		for (size_t i = 0; i < count; i++) {
			vec[i].iov_base = (char *) ary[i].data();
			vec[i].iov_len  = ary[i].size();
			total += ary[i].size();
		}
		return total;
	}
	
	/**
	 * Suppose that the given IO vectors are placed adjacent to each other
	 * in a single contiguous block of memory. Given a position inside this
	 * block of memory, this function will calculate the index of in the IO
	 * vector array and the offset inside that IO vector that corresponds
	 * with the position.
	 *
	 * For example, given the following array of IO vectors:
	 * ["AAA", "BBBB", "CC"]
	 * Position 0 would correspond to the first item, offset 0.
	 * Position 1 would correspond to the first item, offset 1.
	 * Position 5 would correspond to the second item, offset 2.
	 * And so forth.
	 */
	void findDataPositionIndexAndOffset(struct iovec iov[], size_t count, size_t position,
		size_t *index, size_t *offset) const
	{
		size_t i, begin;
		
		begin = 0;
		for (i = 0; OXT_LIKELY(i < count); i++) {
			size_t end = begin + iov[i].iov_len;
			if (OXT_LIKELY(begin <= position)) {
				if (position < end) {
					*index = i;
					*offset = position - begin;
					return;
				} else {
					begin = end;
				}
			} else {
				// Never reached.
				abort();
			}
		}
		// Never reached.
		abort();
	}
	
	/**
	 * To be called when the entire outbox has been successfully sent out.
	 */
	void outboxFlushed(const ClientPtr &client) {
		switch (client->state) {
		case Client::ES_CONNECTED:
			watchReadEvents(client, true);
			watchWriteEvents(client, false);
			break;
		case Client::ES_WRITES_PENDING:
			client->state = Client::ES_CONNECTED;
			watchReadEvents(client, true);
			watchWriteEvents(client, false);
			break;
		case Client::ES_DISCONNECTING_WITH_WRITES_PENDING:
			client->state = Client::ES_DISCONNECTED;
			try {
				client->fd.close();
			} catch (const SystemException &e) {
				logSystemError(client, e.brief(), e.code());
			}
			clients.erase(client);
			break;
		default:
			// Never reached.
			abort();
		}
	}
	
	/**
	 * To be called when the outbox has been partially sent out.
	 */
	void outboxPartiallyFlushed(const ClientPtr &client) {
		switch (client->state) {
		case Client::ES_CONNECTED:
			client->state = Client::ES_WRITES_PENDING;
			// If we have way too much stuff in the outbox then
			// suspend reading until we've sent out the entire outbox.
			watchReadEvents(client, client->outbox.size() < 1024 * 32);
			watchWriteEvents(client, true);
			break;
		case Client::ES_WRITES_PENDING:
		case Client::ES_DISCONNECTING_WITH_WRITES_PENDING:
			watchReadEvents(client, false);
			watchWriteEvents(client, true);
			break;
		default:
			// Never reached.
			abort();
		}
	}
	
	void watchReadEvents(const ClientPtr &client, bool enable = true) {
		if (client->readWatcherStarted && !enable) {
			client->readWatcherStarted = false;
			client->readWatcher.stop();
		} else if (!client->readWatcherStarted && enable) {
			client->readWatcherStarted = true;
			client->readWatcher.start();
		}
	}
	
	void watchWriteEvents(const ClientPtr &client, bool enable = true) {
		if (client->writeWatcherStarted && !enable) {
			client->writeWatcherStarted = false;
			client->writeWatcher.stop();
		} else if (!client->writeWatcherStarted && enable) {
			client->writeWatcherStarted = true;
			client->writeWatcher.start();
		}
	}
	
	void onClientWritable(const ClientPtr &client) {
		if (client->state == Client::ES_DISCONNECTED) {
			return;
		}
		
		this_thread::disable_syscall_interruption dsi;
		size_t sent = 0;
		bool done = client->outbox.empty();
		
		while (!done) {
			ssize_t ret = syscalls::write(client->fd,
				client->outbox.data() + sent,
				client->outbox.size() - sent);
			if (ret == -1) {
				if (errno != EAGAIN) {
					int e = errno;
					disconnect(client, true);
					logSystemError(client, "Cannot write data to client", e);
					return;
				}
				done = true;
			} else {
				sent += ret;
				done = sent == client->outbox.size();
			}
		}
		if (sent > 0) {
			client->outbox.erase(0, sent);
		}
		
		if (client->outbox.empty()) {
			outboxFlushed(client);
		} else {
			outboxPartiallyFlushed(client);
		}
	}
	
	void onAcceptable(ev::io &w, int revents) {
		this_thread::disable_syscall_interruption dsi;
		int i = 0;
		bool done = false;
		
		// Accept at most 100 connections on every accept readiness event
		// in order to give other events the chance to be processed.
		while (i < 100 && !done) {
			// Reserve enough space to hold both a Unix domain socket
			// address and an IP socket address.
			union {
				struct sockaddr_un local;
				struct sockaddr_in inet;
			} addr;
			socklen_t len = sizeof(addr);
			
			int clientfd = syscalls::accept(fd, (struct sockaddr *) &addr, &len);
			if (clientfd == -1) {
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					int e = errno;
					logSystemError("Cannot accept new client", e);
				}
				done = true;
			} else {
				FileDescriptor clientfdGuard = clientfd;
				int optval = 1;
				
				setNonBlocking(clientfdGuard);
				syscalls::setsockopt(clientfd, SOL_SOCKET, SO_KEEPALIVE,
					&optval, sizeof(optval));
				
				ClientPtr client = createClient();
				client->state = Client::ES_CONNECTED;
				client->fd = clientfdGuard;
				client->readWatcher.set<Client, &Client::_onReadable>(client.get());
				client->readWatcher.set(client->fd, ev::READ);
				client->readWatcher.start();
				client->readWatcherStarted = true;
				client->writeWatcher.set<Client, &Client::_onWritable>(client.get());
				client->writeWatcher.set(client->fd, ev::WRITE);
				client->writeWatcherStarted = false;
				clients.insert(client);
				onNewClient(client);
			}
			i++;
		}
	}
	
public:
	EventedServer(struct ev_loop *_loop, FileDescriptor serverFd)
		: loop(_loop),
		  acceptWatcher(_loop)
	{
		fd = serverFd;
		setNonBlocking(serverFd);
		acceptWatcher.set<EventedServer, &EventedServer::onAcceptable>(this);
		acceptWatcher.start(fd, ev::READ);
	}
	
	virtual ~EventedServer() { }
	
	struct ev_loop *getLoop() const {
		return loop;
	}
};


} // namespace Passenger

#endif /* _PASSENGER_EVENTED_SERVER_H_ */