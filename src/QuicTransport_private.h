// Copyright 2011-2020 Wason Technology, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "RobotRaconteur/QuicTransport.h"
#include "ASIOStreamBaseTransport.h"
#include <list>

extern "C" {
#include <picoquic.h>
#include <picosocks.h>
}

#pragma once


namespace RobotRaconteur
{

class QuicTransportConnection : public detail::ASIOStreamBaseTransport
	{
	public:
		
		friend class QuicTransport;

		QuicTransportConnection(RR_SHARED_PTR<QuicTransport> parent, boost::string_ref url, bool server, uint32_t local_endpoint);

		void AsyncAttachSocket(RR_SHARED_PTR<detail::PicoQuicConnection> socket, boost::function<void(RR_SHARED_PTR<RobotRaconteurException>)>& callback);
			
	public:

		virtual void MessageReceived(RR_INTRUSIVE_PTR<Message> m);
	protected:
		virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

		virtual void async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler) ;
		
		virtual size_t available();

		virtual bool IsLargeTransferAuthorized();

				
	public:
		virtual void Close();

	protected:

		void Close1(const boost::system::error_code& ec);

		bool IsClosed();

		void ForceClose();

	public:

		virtual  uint32_t GetLocalEndpoint() ;

		virtual  uint32_t GetRemoteEndpoint() ;

		virtual void CheckConnection(uint32_t endpoint);

		virtual bool IsSecure();

		virtual bool IsSecurePeerIdentityVerified();

		virtual std::string GetSecurePeerIdentity();

	protected:

		RR_SHARED_PTR<detail::PicoQuicConnection> socket;
		boost::mutex socket_lock;
		bool server;
		std::string url;

		RR_WEAK_PTR<QuicTransport> parent;

		uint32_t m_RemoteEndpoint;
		uint32_t m_LocalEndpoint;

		boost::recursive_mutex close_lock;
		bool closing;
		
		
		virtual void StreamOpMessageReceived(RR_INTRUSIVE_PTR<Message> m);

	};

	void QuicTransport_attach_transport(RR_SHARED_PTR<QuicTransport> parent,RR_SHARED_PTR<detail::PicoQuicConnection> socket, boost::string_ref url, bool server, uint32_t endpoint, boost::function<void( RR_SHARED_PTR<detail::PicoQuicConnection> , RR_SHARED_PTR<ITransportConnection> , RR_SHARED_PTR<RobotRaconteurException> )>& callback);

	void QuicTransport_connected_callback2(RR_SHARED_PTR<QuicTransport> parent, RR_SHARED_PTR<detail::PicoQuicConnection> socket, RR_SHARED_PTR<ITransportConnection> connection, RR_SHARED_PTR<RobotRaconteurException> err);

    namespace detail
    {
        class PicoQuicConnection
        {
        public:
            virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler) = 0;

		    virtual void async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler) = 0;

            virtual void close() = 0;

            virtual bool is_open() = 0;

            virtual boost::asio::ip::udp::endpoint local_endpoint();

            virtual ~PicoQuicConnection() {}
        };

        class PicoQuicServer : public RR_ENABLE_SHARED_FROM_THIS<PicoQuicServer>, boost::noncopyable
        {
        public:

            PicoQuicServer(RR_SHARED_PTR<QuicTransport> parent);

            void StartServer(uint16_t port);

            uint16_t GetPort();

            void Close();
        };

        class PicoQuicServerConnection : public RR_ENABLE_SHARED_FROM_THIS<PicoQuicServerConnection>, boost::noncopyable
        {
        public:
            virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

		    virtual void async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

            virtual void close();

            virtual bool is_open();

            virtual ~PicoQuicServerConnection() {}
        };

        class PicoQuicClientConnection : public RR_ENABLE_SHARED_FROM_THIS<PicoQuicClientConnection>, boost::noncopyable
        {
        public:
            virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

		    virtual void async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

            virtual void close();

            virtual bool is_open();

            virtual ~PicoQuicClientConnection() {}
        };


    }

}
