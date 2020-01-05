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

#include "RobotRaconteur/RobotRaconteurNode.h"
#include <boost/shared_array.hpp>

#pragma once

namespace RobotRaconteur
{
	
    class ROBOTRACONTEUR_CORE_API QuicTransportConnection;

    namespace detail
    {
        class PicoQuicConnection;
        class PicoQuicServer;
    }
	
	class ROBOTRACONTEUR_CORE_API QuicTransport : public Transport, public RR_ENABLE_SHARED_FROM_THIS<QuicTransport>
	{
		friend class QuicTransportConnection;
		
	public:
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> > TransportConnections;
		boost::mutex TransportConnections_lock;
		
		std::list<RR_WEAK_PTR<ITransportConnection> > closing_TransportConnections;
		std::list<RR_WEAK_PTR<ITransportConnection> > incoming_TransportConnections;

		QuicTransport(RR_SHARED_PTR<RobotRaconteurNode> node=RobotRaconteurNode::sp());
		
		virtual ~QuicTransport();


	public:
		virtual bool IsServer() const;
		virtual bool IsClient() const;

		
		virtual int32_t GetDefaultReceiveTimeout();
		virtual void SetDefaultReceiveTimeout(int32_t milliseconds);
		virtual int32_t GetDefaultConnectTimeout();
		virtual void SetDefaultConnectTimeout(int32_t milliseconds);

		virtual std::string GetUrlSchemeString() const;

		virtual uint16_t GetListenPort();

		virtual void SendMessage(RR_INTRUSIVE_PTR<Message> m);

		virtual void AsyncSendMessage(RR_INTRUSIVE_PTR<Message> m, boost::function<void (RR_SHARED_PTR<RobotRaconteurException> )>& callback);

		virtual void AsyncCreateTransportConnection(boost::string_ref url, RR_SHARED_PTR<Endpoint> e, boost::function<void (RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException> ) >& callback);


		virtual RR_SHARED_PTR<ITransportConnection> CreateTransportConnection(boost::string_ref url, RR_SHARED_PTR<Endpoint> e);

		virtual void CloseTransportConnection(RR_SHARED_PTR<Endpoint> e);

	protected:

		virtual void CloseTransportConnection_timed(const boost::system::error_code& err, RR_SHARED_PTR<Endpoint> e, RR_SHARED_PTR<void> timer);
		
	public:

		virtual void StartServer(uint16_t porte);
		
		virtual bool CanConnectService(boost::string_ref url);
	
		virtual void Close();

		virtual void CheckConnection(uint32_t endpoint);
				
		virtual void PeriodicCleanupTask();		

		uint32_t TransportCapability(boost::string_ref name);

		virtual void MessageReceived(RR_INTRUSIVE_PTR<Message> m);

		virtual int32_t GetDefaultHeartbeatPeriod();
		virtual void SetDefaultHeartbeatPeriod(int32_t milliseconds);

		virtual int32_t GetMaxMessageSize();
		virtual void SetMaxMessageSize(int32_t size);
		virtual int32_t GetMaxConnectionCount();
		virtual void SetMaxConnectionCount(int32_t count);
		
		virtual bool GetDisableMessage3();
		virtual void SetDisableMessage3(bool d);

		virtual bool GetDisableStringTable();
		virtual void SetDisableStringTable(bool d);

		virtual bool GetDisableAsyncMessageIO();
		virtual void SetDisableAsyncMessageIO(bool d);

		template<typename T, typename F>
		boost::signals2::connection AddCloseListener(RR_SHARED_PTR<T> t, const F& f)
		{
			boost::mutex::scoped_lock lock(closed_lock);
			if (closed)
			{
				lock.unlock();
				boost::bind(f, t) ();
				return boost::signals2::connection();
			}

			return close_signal.connect(boost::signals2::signal<void()>::slot_type(
				boost::bind(f, t.get())
			).track(t));			
		}

	protected:
		
		virtual void register_transport(RR_SHARED_PTR<ITransportConnection> connection);
		virtual void erase_transport(RR_SHARED_PTR<ITransportConnection> connection);
		virtual void incoming_transport(RR_SHARED_PTR<ITransportConnection> connection);

        boost::mutex quic_server_lock;
        RR_SHARED_PTR<detail::PicoQuicServer> quic_server;


		boost::mutex parameter_lock;
		int32_t heartbeat_period;
		int32_t default_connect_timeout;
		int32_t default_receive_timeout;
		int32_t max_message_size;
		int32_t max_connection_count;
		bool disable_message3;
		bool disable_string_table;
		bool disable_async_message_io;
		
		bool closed;
		boost::signals2::signal<void()> close_signal;
		boost::mutex closed_lock;
	};

#ifndef BOOST_NO_CXX11_TEMPLATE_ALIASES
	using QuicTransportPtr = RR_SHARED_PTR<QuicTransport>;
#endif

	
}
