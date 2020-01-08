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

            virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

		    virtual void async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

            virtual void close();

            virtual bool is_open() = 0;

            virtual boost::asio::ip::udp::endpoint local_endpoint() = 0;

            virtual ~PicoQuicConnection() {}

		protected:

			PicoQuicConnection();

			RR_WEAK_PTR<RobotRaconteurNode> node;

			boost::mutex send_recv_lock;
			bool recv_failed;

			const_buffers send_buffer;
			size_t send_buffer_size;
			boost::function<void(boost::system::error_code,size_t)> send_handler;
			bool can_send;

			mutable_buffers recv_buffer;
			std::deque<boost::tuple<boost::asio::const_buffer,boost::shared_array<uint8_t> > > early_recv_data;
			boost::function<void(boost::system::error_code,size_t)> recv_handler;

			bool close_requested;
			bool close_fin_sent;

			int do_picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    			size_t length, picoquic_call_back_event_t fin_or_event);

			virtual void wake_quic() = 0;

			virtual int do_recv_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length, bool fin);

			int do_send_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length);

			int do_stream_reset(uint64_t stream_id);

			int do_stream_stop_sending(uint64_t stream);

			virtual int do_ready();

			virtual void connection_closed();
        };

        class PicoQuicServer : public RR_ENABLE_SHARED_FROM_THIS<PicoQuicServer>, boost::noncopyable
        {
        public:
			friend class PicoQuicServerConnection;

            PicoQuicServer(RR_SHARED_PTR<QuicTransport> parent);

            void StartServer(uint16_t port);

            uint16_t GetPort();

            void Close();

		protected:
			RR_WEAK_PTR<QuicTransport> parent;
			RR_WEAK_PTR<RobotRaconteurNode> node;
			uint16_t port;			
			boost::mutex quic_lock;
			bool keep_going;

			boost::shared_ptr<boost::asio::ip::udp::socket> v4_socket_;
			boost::shared_ptr<boost::asio::ip::udp::socket> v6_socket_;    
			picoquic_server_sockets_t server_sockets;
			boost::shared_ptr<boost::asio::deadline_timer> quic_timer_;

			boost::shared_ptr<picoquic_quic_t> qserver;

			RR_BOOST_ASIO_IO_CONTEXT quic_io_context; 
			boost::thread quic_thread;

			static int picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    		size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx);

			void handle_socket_failure(const boost::system::error_code& ec);

			void handle_quic_failure(int ret);

			void begin_v4_wait();
			void handle_v4_wait(boost::system::error_code ec);
			void begin_v6_wait();
			void handle_v6_wait(boost::system::error_code ec);
			void quic_timer_handler(boost::system::error_code ec);

			void run_loop();
			void do_quic();
			void wake_quic();

        };

        class PicoQuicServerConnection : public PicoQuicConnection, public RR_ENABLE_SHARED_FROM_THIS<PicoQuicServerConnection>, boost::noncopyable
        {
        public:

			friend class PicoQuicServer;

			PicoQuicServerConnection(RR_SHARED_PTR<PicoQuicServer> parent, picoquic_cnx_t* cnx);

            virtual bool is_open();

			virtual boost::asio::ip::udp::endpoint local_endpoint();

            virtual ~PicoQuicServerConnection() {}

		protected:

			static int picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    			size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx);

			RR_WEAK_PTR<PicoQuicServer> parent;			

			picoquic_cnx_t* cnx_client;

			virtual void wake_quic();

			bool mark_active_streams();
        };

        class PicoQuicClientConnection : public PicoQuicConnection, public RR_ENABLE_SHARED_FROM_THIS<PicoQuicClientConnection>, boost::noncopyable
        {
        public:

			PicoQuicClientConnection(RR_SHARED_PTR<RobotRaconteurNode> node);

            virtual void async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler);

		    virtual bool is_open();

			virtual void async_connect(boost::asio::ip::udp::endpoint server_ep, const std::vector<std::string>& sni, boost::function<void(boost::system::error_code)> handler);

			virtual void close();

			virtual boost::asio::ip::udp::endpoint local_endpoint();

            virtual ~PicoQuicClientConnection() {}

		protected:

			static int picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    			size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx);

			bool connect_started;
			bool can_send;
			bool keep_going;

			RR_BOOST_ASIO_IO_CONTEXT quic_io_context;
			boost::shared_ptr<boost::asio::ip::udp::socket> socket_;
			boost::shared_ptr<boost::asio::deadline_timer> quic_timer_;

			boost::shared_ptr<picoquic_quic_t> qclient;

			picoquic_cnx_t* cnx_client;

			boost::mutex quic_lock;
			boost::thread quic_thread;

			boost::function<void(boost::system::error_code)> connect_handler;

			bool has_sent_first;

			void handle_socket_failure(const boost::system::error_code& ec);

			void handle_quic_failure(int ret);

			void begin_socket_wait();

			void handle_socket_wait(boost::system::error_code ec);

			void quic_timer_handler(boost::system::error_code ec);

			void run_loop();

			void do_quic();

			virtual void wake_quic();

			virtual int do_recv_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length, bool fin);

			virtual int do_ready();
        };
    }

	namespace detail
	{
		class QuicConnector : public RR_ENABLE_SHARED_FROM_THIS<QuicConnector>
		{
		public:
			QuicConnector(RR_SHARED_PTR<QuicTransport> parent);

			void Connect(std::vector<std::string> url, uint32_t endpoint, boost::function<void(RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback);
			

		protected:
			
#if BOOST_ASIO_VERSION < 101200
			void connect2(int32_t key, const boost::system::error_code& err, boost::asio::ip::basic_resolver_iterator<boost::asio::ip::udp> endpoint_iterator, boost::function<void(RR_SHARED_PTR<QuicTransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback);
#else
			void connect2(int32_t key, const boost::system::error_code& err, boost::asio::ip::udp::resolver::results_type results, boost::function<void(RR_SHARED_PTR<QuicTransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback);
#endif

			void connected_callback(RR_SHARED_PTR<PicoQuicConnection> socket, RR_SHARED_PTR<boost::signals2::scoped_connection> socket_closer, int32_t key, const boost::system::error_code& error);

			void connected_callback2(RR_SHARED_PTR<PicoQuicConnection> socket, int32_t key, RR_SHARED_PTR<ITransportConnection> connection, RR_SHARED_PTR<RobotRaconteurException> err);

			void connect_timer_callback(const boost::system::error_code& e);

			RR_SHARED_PTR<QuicTransport> parent;

			RR_SHARED_PTR<boost::asio::deadline_timer> connect_timer;
			
			bool connecting;

			boost::mutex connecting_lock;

			boost::function<void(RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback;

			RR_SHARED_PTR<boost::asio::ip::udp::resolver> _resolver;

			uint32_t endpoint;

			int32_t resolve_count;
			int32_t connect_count;
						
			std::list<int32_t> active;
			int32_t active_count;
			std::list<RR_SHARED_PTR<RobotRaconteurException> > errors;

			void handle_error(const int32_t& key, const boost::system::error_code& err);
			void handle_error(const int32_t& key, RR_SHARED_PTR<RobotRaconteurException> err);

			bool socket_connected;

			boost::mutex this_lock;
			std::string url;
			std::vector<std::string> sni;

			RR_WEAK_PTR<RobotRaconteurNode> node;			
		public:

		};
	}

}
