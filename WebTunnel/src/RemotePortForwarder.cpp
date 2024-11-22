//
// RemotePortForwarder.cpp
//
// Library: WebTunnel
// Package: WebTunnel
// Module:  RemotePortForwarder
//
// Copyright (c) 2013, Applied Informatics Software Engineering GmbH.
// All rights reserved.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "Poco/WebTunnel/RemotePortForwarder.h"
#include "Poco/WebTunnel/Protocol.h"
#include "Poco/Net/NetException.h"
#include "Poco/Format.h"
#include "Poco/BinaryWriter.h"
#include "Poco/MemoryStream.h"
#include "Poco/CountingStream.h"
#include <cstring>


using namespace std::string_literals;


namespace Poco {
namespace WebTunnel {


//
// SocketFactory
//


SocketFactory::SocketFactory()
{
}


SocketFactory::~SocketFactory()
{
}


Poco::Net::StreamSocket SocketFactory::createSocket(const Poco::Net::SocketAddress& addr)
{
	Poco::Net::StreamSocket streamSocket;
	streamSocket.connectNB(addr);
	return streamSocket;
}


//
// RemotePortForwarder
//


RemotePortForwarder::RemotePortForwarder(SocketDispatcher& dispatcher, Poco::SharedPtr<Poco::Net::WebSocket> pWebSocket, const Poco::Net::IPAddress& host, const std::set<Poco::UInt16>& ports, Poco::Timespan remoteTimeout, SocketFactory::Ptr pSocketFactory):
	_dispatcher(dispatcher),
	_pSocketFactory(pSocketFactory),
	_pWebSocket(pWebSocket),
	_host(host),
	_ports(ports),
	_connectTimeout(30, 0),
	_localTimeout(7200, 0),
	_closeTimeout(30, 0),
	_remoteTimeout(remoteTimeout),
	_logger(Poco::Logger::get("WebTunnel.RemotePortForwarder"s))
{
	pWebSocket->setBlocking(false);
	_dispatcher.addSocket(*pWebSocket, new TunnelDemultiplexer(*this), Poco::Net::PollSet::POLL_READ, remoteTimeout);
}


RemotePortForwarder::~RemotePortForwarder()
{
	try
	{
		stop();
	}
	catch (...)
	{
		poco_unexpected();
	}
}


void RemotePortForwarder::stop()
{
	const Poco::Timestamp::TimeDiff STOP_TIMEOUT = 500000;

	if (_dispatcher.hasSocket(*_pWebSocket))
	{
		_dispatcher.queueTask(
			[pSelf=this](SocketDispatcher& dispatcher)
			{
				pSelf->closeWebSocket(RPF_CLOSE_GRACEFUL, true);
			}
		);

		_logger.debug("Waiting for WebSocket closing handshake to complete..."s);
		Poco::Timestamp closeTime;
		while (_dispatcher.hasSocket(*_pWebSocket) && !closeTime.isElapsed(STOP_TIMEOUT))
		{
			Poco::Thread::sleep(20);
		}
		_dispatcher.removeSocket(*_pWebSocket);
	}
}


void RemotePortForwarder::setLocalTimeout(const Poco::Timespan& timeout)
{
	_localTimeout = timeout;
}


const Poco::Timespan& RemotePortForwarder::getLocalTimeout() const
{
	return _localTimeout;
}


void RemotePortForwarder::setCloseTimeout(const Poco::Timespan& timeout)
{
	_closeTimeout = timeout;
}


const Poco::Timespan& RemotePortForwarder::getCloseTimeout() const
{
	return _closeTimeout;
}


void RemotePortForwarder::setConnectTimeout(const Poco::Timespan& timeout)
{
	_connectTimeout = timeout;
}


const Poco::Timespan& RemotePortForwarder::getConnectTimeout() const
{
	return _connectTimeout;
}


const Poco::Timespan& RemotePortForwarder::remoteTimeout() const
{
	return _remoteTimeout;
}


void RemotePortForwarder::multiplex(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel, Poco::Buffer<char>& buffer)
{
	std::size_t hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_DATA, 0, channel);
	int n = 0;
	try
	{
		n = socket.receiveBytes(buffer.begin() + hn, static_cast<int>(buffer.size() - hn));
		if (n == 0)
		{
			if (_logger.debug())
			{
				_logger.debug("Actively closing channel %hu."s, channel);
			}
			dispatcher.updateSocket(socket, 0, _closeTimeout);
			if (setChannelFlag(channel, CF_CLOSED_LOCAL) & CF_CLOSED_REMOTE)
			{
				_logger.debug("Channel %hu also already closed by remote peer."s, channel);
				removeChannel(channel);
			}
			hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_CLOSE, 0, channel);
		}
		else if (n < 0)
		{
			// polled readable, but no payload data received, as may happen with TLS
			return;
		}
	}
	catch (Poco::Exception& exc)
	{
		removeChannel(channel);
		n = 0;
		// Workaround for some HTTPS servers that do not orderly close a TLS connection.
		if (std::strcmp(exc.name(), "SSL connection unexpectedly closed") == 0)
		{
			hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_CLOSE, 0, channel);
		}
		else
		{
			_logger.error("Error reading from locally forwarded socket for channel %hu: %s"s, channel, exc.displayText());
			hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_ERROR, 0, channel, Protocol::WT_ERR_SOCKET);
		}
	}
	try
	{
		dispatcher.sendBytes(*_pWebSocket, buffer.begin(), static_cast<int>(n + hn), Poco::Net::WebSocket::FRAME_BINARY);
	}
	catch (Poco::Exception& exc)
	{
		_logger.error("Error sending WebSocket frame for channel %hu: %s"s, channel, exc.displayText());
		closeWebSocket(RPF_CLOSE_ERROR, false);
	}
}


void RemotePortForwarder::multiplexError(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel, Poco::Buffer<char>& buffer)
{
	_logger.error("Error reading from local socket for channel %hu"s, channel);
	removeChannel(channel);
	std::size_t hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_ERROR, 0, channel, Protocol::WT_ERR_SOCKET);
	try
	{
		dispatcher.sendBytes(*_pWebSocket, buffer.begin(), static_cast<int>(hn), Poco::Net::WebSocket::FRAME_BINARY);
	}
	catch (Poco::Exception& exc)
	{
		_logger.error("Error sending WebSocket error frame for channel %hu: %s"s, channel, exc.displayText());
		closeWebSocket(RPF_CLOSE_ERROR, false);
	}
}


void RemotePortForwarder::multiplexTimeout(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel, Poco::Buffer<char>& buffer)
{
	if (!(getChannelFlags(channel) & CF_CLOSED_LOCAL))
	{
		_logger.error("Timeout reading from local socket for channel %hu"s, channel);
		std::size_t hn = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_ERROR, 0, channel, Protocol::WT_ERR_TIMEOUT);
		try
		{
			dispatcher.sendBytes(*_pWebSocket, buffer.begin(), static_cast<int>(hn), Poco::Net::WebSocket::FRAME_BINARY);
		}
		catch (Poco::Exception& exc)
		{
			_logger.error("Error sending WebSocket error frame for channel %hu: %s"s, channel, exc.displayText());
			closeWebSocket(RPF_CLOSE_ERROR, false);
		}
	}
	removeChannel(channel);
}


void RemotePortForwarder::demultiplex(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::Buffer<char>& buffer)
{
	int wsFlags;
	int n = 0;
	try
	{
		n = _pWebSocket->receiveFrame(buffer.begin(), static_cast<int>(buffer.size()), wsFlags);
		if (n < 0) return;
	}
	catch (Poco::Exception& exc)
	{
		_logger.error("Error receiving WebSocket frame: %s"s, exc.displayText());
		closeWebSocket(RPF_CLOSE_ERROR, false);
		return;
	}
	if ((wsFlags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_PONG)
	{
		_logger.debug("PONG received"s);
		_timeoutCount = 0;
		return;
	}
	if (n > 0 && (wsFlags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_BINARY)
	{
		Poco::UInt8 opcode;
		Poco::UInt8 flags;
		Poco::UInt16 channel;
		Poco::UInt16 portOrErrorCode;
		std::size_t hn = Protocol::readHeader(buffer.begin(), buffer.size(), opcode, flags, channel, &portOrErrorCode);
		switch (opcode)
		{
		case Protocol::WT_OP_DATA:
			forwardData(buffer.begin() + hn, static_cast<int>(n - hn), channel);
			break;

		case Protocol::WT_OP_OPEN_REQUEST:
			openChannel(channel, portOrErrorCode);
			break;

		case Protocol::WT_OP_CLOSE:
			if (_logger.debug())
			{
				_logger.debug("Passively closing channel %hu."s, channel);
			}
			if (setChannelFlag(channel, CF_CLOSED_REMOTE) & CF_CLOSED_LOCAL)
			{
				_logger.debug("Channel %hu also already closed by local peer."s, channel);
				removeChannel(channel);
			}
			else
			{
				shutdownSendChannel(channel);
			}
			break;

		case Protocol::WT_OP_ERROR:
			_logger.error("Status %hu reported by peer. Closing channel %hu."s, portOrErrorCode, channel);
			removeChannel(channel);
			break;

		default:
			_logger.error("Invalid WebSocket frame received (bad opcode: %hu)."s, static_cast<Poco::UInt16>(opcode));
			sendResponse(channel, Protocol::WT_OP_ERROR, Protocol::WT_ERR_PROTOCOL);
			break;
		}
	}
	else if (n == 0 && wsFlags == 0)
	{
		if (!(_webSocketFlags & CF_CLOSED_REMOTE))
		{
			_logger.debug("Peer has ungracefully closed WebSocket."s);
		}
		_webSocketFlags |= CF_CLOSED_REMOTE;
		if (_webSocketFlags & CF_CLOSED_LOCAL)
		{
			_dispatcher.removeSocket(*_pWebSocket);
		}
		else
		{
			closeWebSocket(RPF_CLOSE_ERROR, false);
		}
	}
	else if (n == 0 && (wsFlags & Poco::Net::WebSocket::FRAME_OP_BITMASK) == Poco::Net::WebSocket::FRAME_OP_CLOSE)
	{
		_logger.debug("Peer has gracefully closed WebSocket."s);
		_webSocketFlags |= CF_CLOSED_REMOTE;
		if (_webSocketFlags & CF_CLOSED_LOCAL)
		{
			_dispatcher.removeSocket(*_pWebSocket);
		}
		else
		{
			closeWebSocket(RPF_CLOSE_GRACEFUL, true);
		}
	}
	else
	{
		_logger.debug("Ignoring unsupported frame opcode."s);
	}
}


void RemotePortForwarder::demultiplexError(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::Buffer<char>& buffer)
{
	_logger.error("Error reading from WebSocket."s);
	if (_webSocketFlags & CF_CLOSED_LOCAL)
	{
		_dispatcher.removeSocket(*_pWebSocket);
	}
	else
	{
		closeWebSocket(RPF_CLOSE_ERROR, false);
	}
}


void RemotePortForwarder::demultiplexTimeout(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::Buffer<char>& buffer)
{
	_logger.debug("Timeout reading from WebSocket."s);
	if (_timeoutCount == 0)
	{
		_timeoutCount = 1;
		try
		{
			_logger.debug("Sending PING."s);
			dispatcher.sendBytes(*_pWebSocket, 0, 0, Poco::Net::WebSocket::FRAME_FLAG_FIN | Poco::Net::WebSocket::FRAME_OP_PING);
		}
		catch (Poco::Exception&)
		{
			closeWebSocket(RPF_CLOSE_ERROR, false);
		}
	}
	else
	{
		closeWebSocket(RPF_CLOSE_TIMEOUT, false);
	}
}


void RemotePortForwarder::connect(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel)
{
	socket.setNoDelay(true);
	try
	{
		sendResponse(channel, Protocol::WT_OP_OPEN_CONFIRM, 0);
	}
	catch (Poco::Exception& exc)
	{
		_logger.error("Failed to send open confirmation for channel %hu: %s"s, channel, exc.displayText());
		return;
	}

	_dispatcher.removeSocket(socket);
	SocketDispatcher::SocketHandler::Ptr pMultiplexer = new TunnelMultiplexer(*this, channel);
	_dispatcher.addSocket(socket, pMultiplexer, Poco::Net::PollSet::POLL_READ, _localTimeout);
}


void RemotePortForwarder::connectError(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel)
{
	_dispatcher.removeSocket(socket);
	int rc = socket.impl()->socketError();
	if (rc == POCO_ECONNREFUSED)
		sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_CONN_REFUSED);
	else
		sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_SOCKET);
	removeChannel(channel);
}


void RemotePortForwarder::connectTimeout(SocketDispatcher& dispatcher, Poco::Net::StreamSocket& socket, Poco::UInt16 channel)
{
	_dispatcher.removeSocket(socket);
	sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_TIMEOUT);
	removeChannel(channel);
}


void RemotePortForwarder::forwardData(const char* buffer, int size, Poco::UInt16 channel)
{
	Poco::ScopedLockWithUnlock<Poco::FastMutex> lock(_mutex);
	ChannelMap::iterator it = _channelMap.find(channel);
	if (it != _channelMap.end())
	{
		Poco::Net::StreamSocket streamSocket = it->second.socket;
		lock.unlock();
		try
		{
			_dispatcher.sendBytes(streamSocket, buffer, size, 0);
		}
		catch (Poco::Exception&)
		{
			removeChannel(channel);
			sendResponse(channel, Protocol::WT_OP_ERROR, Protocol::WT_ERR_SOCKET);
		}
	}
	else
	{
		_logger.warning("Forwarding request for invalid channel: %hu."s, channel);
		lock.unlock();
		sendResponse(channel, Protocol::WT_OP_ERROR, Protocol::WT_ERR_BAD_CHANNEL);
	}
}


void RemotePortForwarder::openChannel(Poco::UInt16 channel, Poco::UInt16 port)
{
	if (_ports.find(port) == _ports.end())
	{
		_logger.warning("Open channel request for invalid port: %hu (channel %hu)."s, port, channel);

		sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_NOT_FORWARDED);
		return;
	}

	Poco::ScopedLockWithUnlock<Poco::FastMutex> lock(_mutex);
	ChannelMap::iterator it = _channelMap.find(channel);
	if (it == _channelMap.end())
	{
		if (_logger.debug())
		{
			_logger.debug("Opening channel %hu to port %hu on %s."s, channel, port, _host.toString());
		}
		try
		{
			Poco::Net::SocketAddress addr(_host, port);
			Poco::Net::StreamSocket streamSocket(_pSocketFactory->createSocket(addr));
			SocketDispatcher::SocketHandler::Ptr pMultiplexer = new TunnelConnector(*this, channel);
			_dispatcher.addSocket(streamSocket, pMultiplexer, Poco::Net::PollSet::POLL_WRITE, _connectTimeout);
			_channelMap[channel] = {streamSocket, 0};
		}
		catch (Poco::Exception& exc)
		{
			lock.unlock();
			_logger.error("Failed to open channel %hu to port %hu at %s: %s"s, channel, port, _host.toString(), exc.displayText());
			sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_SOCKET);
		}
	}
	else
	{
		_logger.warning("Open request for existing channel %hu to port %hu."s, channel, port);
		lock.unlock();
		sendResponse(channel, Protocol::WT_OP_OPEN_FAULT, Protocol::WT_ERR_CHANNEL_IN_USE);
	}
}


void RemotePortForwarder::shutdownSendChannel(Poco::UInt16 channel)
{
	Poco::FastMutex::ScopedLock lock(_mutex);
	ChannelMap::iterator it = _channelMap.find(channel);
	if (it != _channelMap.end())
	{
		_dispatcher.shutdownSend(it->second.socket);
	}
}


void RemotePortForwarder::removeChannel(Poco::UInt16 channel)
{
	Poco::FastMutex::ScopedLock lock(_mutex);
	ChannelMap::iterator it = _channelMap.find(channel);
	if (it != _channelMap.end())
	{
		_dispatcher.closeSocket(it->second.socket);
		_channelMap.erase(it);
	}
}


int RemotePortForwarder::setChannelFlag(Poco::UInt16 channel, int flag)
{
	Poco::FastMutex::ScopedLock lock(_mutex);
	auto it = _channelMap.find(channel);
	if (it != _channelMap.end())
	{
		it->second.flags |= flag;
		return it->second.flags;
	}
	else return 0;
}


int RemotePortForwarder::getChannelFlags(Poco::UInt16 channel) const
{
	Poco::FastMutex::ScopedLock lock(_mutex);
	auto it = _channelMap.find(channel);
	if (it != _channelMap.end())
	{
		return it->second.flags;
	}
	else return 0;
}


void RemotePortForwarder::sendResponse(Poco::UInt16 channel, Poco::UInt8 opcode, Poco::UInt16 errorCode)
{
	char buffer[6];
	std::size_t hn = Protocol::writeHeader(buffer, sizeof(buffer), opcode, 0, channel, errorCode);
	try
	{
		_dispatcher.sendBytes(*_pWebSocket, buffer, hn, Poco::Net::WebSocket::FRAME_BINARY);
	}
	catch (Poco::Exception&)
	{
		closeWebSocket(RPF_CLOSE_ERROR, false);
	}
}


void RemotePortForwarder::closeWebSocket(CloseReason reason, bool active)
{
	if (_webSocketFlags & CF_CLOSED_LOCAL) return;

	if (_logger.debug())
	{
		_logger.debug("Closing WebSocket, reason: %d, active: %b"s, static_cast<int>(reason), active);
	}
	try
	{
		if (reason == RPF_CLOSE_GRACEFUL)
		{
			try
			{
				if (active)
				{
					char buffer[2];
					Poco::MemoryOutputStream ostr(buffer, sizeof(buffer));
					Poco::BinaryWriter writer(ostr, Poco::BinaryWriter::NETWORK_BYTE_ORDER);
					writer << static_cast<Poco::UInt16>(Poco::Net::WebSocket::WS_NORMAL_CLOSE);
					_dispatcher.sendBytes(*_pWebSocket, buffer, sizeof(buffer), Poco::Net::WebSocket::FRAME_FLAG_FIN | Poco::Net::WebSocket::FRAME_OP_CLOSE);
				}
			}
			catch (Poco::Exception&)
			{
			}
		}
		for (ChannelMap::iterator it = _channelMap.begin(); it != _channelMap.end(); ++it)
		{
			_dispatcher.removeSocket(it->second.socket);
		}
		_channelMap.clear();
		_dispatcher.shutdownSend(*_pWebSocket);
	}
	catch (Poco::Exception& exc)
	{
		_logger.log(exc);
	}

	_dispatcher.updateSocket(*_pWebSocket, Poco::Net::PollSet::POLL_READ, _closeTimeout);
	_webSocketFlags |= CF_CLOSED_LOCAL;
	int eventArg = reason;
	webSocketClosed(this, eventArg);
}


namespace
{
	void writeProperties(Poco::BinaryWriter& writer, const std::map<std::string, std::string>& props)
	{
		writer << static_cast<Poco::UInt8>(props.size());
		for (std::map<std::string, std::string>::const_iterator it = props.begin(); it != props.end(); ++it)
		{
			writer << it->first << it->second;
		}
	}
}


void RemotePortForwarder::updateProperties(const std::map<std::string, std::string>& props)
{
	poco_assert (props.size() < 256);

	Poco::CountingOutputStream counterStream;
	Poco::BinaryWriter counterWriter(counterStream, Poco::BinaryWriter::NETWORK_BYTE_ORDER);
	writeProperties(counterWriter, props);
	std::size_t payloadSize = static_cast<std::size_t>(counterStream.chars());

	Poco::Buffer<char> buffer(payloadSize + Protocol::WT_FRAME_HEADER_SIZE);
	std::size_t offset = Protocol::writeHeader(buffer.begin(), buffer.size(), Protocol::WT_OP_PROP_UPDATE, 0, 0);
	Poco::MemoryOutputStream bufferStream(buffer.begin() + offset, buffer.size() - offset);
	Poco::BinaryWriter bufferWriter(bufferStream, Poco::BinaryWriter::NETWORK_BYTE_ORDER);
	writeProperties(bufferWriter, props);

	_dispatcher.sendBytes(*_pWebSocket, buffer.begin(), static_cast<int>(payloadSize + Protocol::WT_FRAME_HEADER_SIZE), Poco::Net::WebSocket::FRAME_BINARY);
}


} } // namespace Poco::WebTunnel
