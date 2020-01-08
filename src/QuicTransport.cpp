// Copyright 2011-2019 Wason Technology, LLC
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
#include "QuicTransport_private.h"
#include <boost/algorithm/string.hpp>
#include <boost/shared_array.hpp>
#include "RobotRaconteur/StringTable.h"
#include "RobotRaconteur/Service.h"
#include "RobotRaconteur/TcpTransport.h"

#include <set>

#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <boost/foreach.hpp>

#if BOOST_ASIO_VERSION < 101200
#define RR_BOOST_ASIO_IP_ADDRESS_V6_FROM_STRING boost::asio::ip::address_v6::from_string
#else
#define RR_BOOST_ASIO_IP_ADDRESS_V6_FROM_STRING boost::asio::ip::make_address_v6
#endif

namespace RobotRaconteur
{

QuicTransport::QuicTransport(RR_SHARED_PTR<RobotRaconteurNode> node)
	: Transport(node)
{
	if (!node) throw InvalidArgumentException("Node cannot be null");

	default_connect_timeout=5000;
	default_receive_timeout=15000;
	max_message_size = 12 * 1024 * 1024;
	max_connection_count = 0;
	this->node=node;
	this->heartbeat_period=10000;

	

#ifndef ROBOTRACONTEUR_DISABLE_MESSAGE3
	disable_message3 = false;
#else
	disable_message3 = true;
#endif
#ifndef ROBOTRACONTEUR_DISABLE_STRINGTABLE
	disable_string_table = false;
#else
	disable_string_table = true;
#endif
	disable_async_message_io = false;
	closed = false;

}

QuicTransport::~QuicTransport()
{

}


void QuicTransport::Close()
{
	{
		boost::mutex::scoped_lock lock(closed_lock);
		if (closed) return;
		closed = true;
	}

	try
	{
		boost::mutex::scoped_lock lock(quic_server_lock);
		if (quic_server) quic_server->Close();		
	}
	catch (std::exception&) {}

	std::vector<RR_SHARED_PTR<ITransportConnection> > t;

	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		boost::copy(TransportConnections | boost::adaptors::map_values, std::back_inserter(t));		
		BOOST_FOREACH (RR_WEAK_PTR<ITransportConnection>& e, incoming_TransportConnections)
		{
			RR_SHARED_PTR<ITransportConnection> t2=e.lock();
			if (t2) t.push_back(t2); 
		}
	}

	BOOST_FOREACH (RR_SHARED_PTR<ITransportConnection>& e, t)
	{
		try
		{
			e->Close();
			RR_SHARED_PTR<QuicTransportConnection> tt=RR_DYNAMIC_POINTER_CAST<QuicTransportConnection>(e);
			if (!tt->IsClosed())
			{
				closing_TransportConnections.push_back(tt);
			}
		}
		catch (std::exception&) {}
	}	
	
	
	boost::posix_time::ptime t1=boost::posix_time::microsec_clock::universal_time();
	boost::posix_time::ptime t2=t1;

	while ((t2-t1).total_milliseconds() < 500)
	{
		bool stillopen=false;
		BOOST_FOREACH (RR_WEAK_PTR<ITransportConnection>& e, closing_TransportConnections)
		{
			try
			{
				RR_SHARED_PTR<ITransportConnection> t1=e.lock();
				if (!t1) continue;
				RR_SHARED_PTR<QuicTransportConnection> t2=RR_DYNAMIC_POINTER_CAST<QuicTransportConnection>(t1);
				if (!t2->IsClosed())
				{
					stillopen=true;
				}
			}
			catch (std::exception&) {}
		}

		if (!stillopen) return;
		boost::this_thread::sleep(boost::posix_time::milliseconds(25));
		t2=boost::posix_time::microsec_clock::universal_time();
	}

	BOOST_FOREACH (RR_WEAK_PTR<ITransportConnection>& e, closing_TransportConnections)
	{
		try
		{
			RR_SHARED_PTR<ITransportConnection> t1=e.lock();
			if (!t1) continue;
			RR_SHARED_PTR<QuicTransportConnection> t2=RR_DYNAMIC_POINTER_CAST<QuicTransportConnection>(t1);
			if (!t2->IsClosed())
			{
				t2->ForceClose();
			}
		}
		catch (std::exception&) {}
	}

	close_signal();
}

bool QuicTransport::IsServer() const
{
	return true;
}

bool QuicTransport::IsClient() const
{
	return true;
}

std::string QuicTransport::GetUrlSchemeString() const
{
	return "rr+quic";
}

uint16_t QuicTransport::GetListenPort()
{
    boost::mutex::scoped_lock(quic_server_lock);
    if (quic_server)
    {
        return quic_server->GetPort();
    }
	return 0;
}

bool QuicTransport::CanConnectService(boost::string_ref url)
{
		
	if (boost::starts_with(url, "rr+quic://"))
		return true;

	return false;
}

void QuicTransport::AsyncCreateTransportConnection(boost::string_ref url, RR_SHARED_PTR<Endpoint> e, boost::function<void (RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException> ) >& callback)
{
	{
		int32_t max_connections = GetMaxConnectionCount();
		if (max_connections > 0)
		{
			boost::mutex::scoped_lock lock(TransportConnections_lock);
			if (boost::numeric_cast<int32_t>(TransportConnections.size()) > max_connections) throw ConnectionException("Too many active QUIC connections");
		}
	}

    
	RR_SHARED_PTR<detail::QuicConnector> c=RR_MAKE_SHARED<detail::QuicConnector>(shared_from_this());
	std::vector<std::string> url2;
	url2.push_back(url.to_string());
	c->Connect(url2,e->GetLocalEndpoint(),callback);
}   

RR_SHARED_PTR<ITransportConnection> QuicTransport::CreateTransportConnection(boost::string_ref url, RR_SHARED_PTR<Endpoint> e)
{
	RR_SHARED_PTR<detail::sync_async_handler<ITransportConnection> > d=RR_MAKE_SHARED<detail::sync_async_handler<ITransportConnection> >(RR_MAKE_SHARED<ConnectionException>("Timeout exception"));
	
	boost::function<void(RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > h = boost::bind(&detail::sync_async_handler<ITransportConnection>::operator(), d, _1, _2);
	AsyncCreateTransportConnection(url, e, h);

	return d->end();

}

void QuicTransport::CloseTransportConnection(RR_SHARED_PTR<Endpoint> e)
{
	
	RR_SHARED_PTR<ServerEndpoint> e2=boost::dynamic_pointer_cast<ServerEndpoint>(e);
	if (e2)
	{
		{
			try
			{
				boost::mutex::scoped_lock lock(TransportConnections_lock);
				RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(e->GetLocalEndpoint());
				if (e1 != TransportConnections.end())
				{
					closing_TransportConnections.push_back(e1->second);
				}
			}
			catch (std::exception&) {}
		}
		RR_SHARED_PTR<boost::asio::deadline_timer> timer(new boost::asio::deadline_timer(GetNode()->GetThreadPool()->get_io_context()));
		timer->expires_from_now(boost::posix_time::milliseconds(100));
		RobotRaconteurNode::asio_async_wait(node, timer, boost::bind(&QuicTransport::CloseTransportConnection_timed, shared_from_this(),boost::asio::placeholders::error,e,timer));
		return;
	}


	RR_SHARED_PTR<ITransportConnection> t;
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(e->GetLocalEndpoint());
		if (e1 == TransportConnections.end()) return;
		t = e1->second;
		TransportConnections.erase(e1);
	}

	if (t)
	{
		try
		{
			t->Close();
		}
		catch (std::exception&) {}

		RR_SHARED_PTR<QuicTransportConnection> tt=RR_DYNAMIC_POINTER_CAST<QuicTransportConnection>(t);
		if (tt)
		{
			if (!tt->IsClosed())
			{
				boost::mutex::scoped_lock lock(TransportConnections_lock);
				closing_TransportConnections.push_back(t);
			}
		}
	}
	
}

void QuicTransport::CloseTransportConnection_timed(const boost::system::error_code& err, RR_SHARED_PTR<Endpoint> e,RR_SHARED_PTR<void> timer)
{
	if (err) return;

	RR_SHARED_PTR<ITransportConnection> t;
	
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(e->GetLocalEndpoint());
		if (e1 == TransportConnections.end()) return;
		t = e1->second;
	}
	
	if (t)
	{
		try
		{
			t->Close();
		}
		catch (std::exception&) {}
	}

}

void QuicTransport::SendMessage(RR_INTRUSIVE_PTR<Message> m)
{
	

	RR_SHARED_PTR<ITransportConnection> t;
	
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(m->header->SenderEndpoint);
		if (e1 == TransportConnections.end()) throw ConnectionException("Transport connection to remote host not found");
		t = e1->second;
	}	

	t->SendMessage(m);
}

uint32_t QuicTransport::TransportCapability(boost::string_ref name)
{
	return 0;
}

void QuicTransport::PeriodicCleanupTask()
{
	boost::mutex::scoped_lock lock(TransportConnections_lock);
	for (RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e=TransportConnections.begin(); e!=TransportConnections.end(); )
	{
		try
		{
			RR_SHARED_PTR<QuicTransportConnection> e2=rr_cast<QuicTransportConnection>(e->second);
			if (!e2->IsConnected())
			{
				e=TransportConnections.erase(e);
			}
			else
			{
				e++;
			}
		}
		catch (std::exception&) {}
	}

	for (std::list<RR_WEAK_PTR<ITransportConnection> >::iterator e=closing_TransportConnections.begin(); e!=closing_TransportConnections.end();)
	{
		try
		{
			if (e->expired())
			{
				e=closing_TransportConnections.erase(e);
			}
			else
			{
				e++;
			}
		}
		catch (std::exception&) {}
	}

	for (std::list<RR_WEAK_PTR<ITransportConnection> >::iterator e=incoming_TransportConnections.begin(); e!=incoming_TransportConnections.end();)
	{
		try
		{
			if (e->expired())
			{
				e=incoming_TransportConnections.erase(e);
			}
			else
			{
				e++;
			}
		}
		catch (std::exception&) {}
	}
}

void QuicTransport::AsyncSendMessage(RR_INTRUSIVE_PTR<Message> m, boost::function<void (RR_SHARED_PTR<RobotRaconteurException> )>& handler)
{	

	RR_SHARED_PTR<ITransportConnection> t;
	
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(m->header->SenderEndpoint);
		if (e1 == TransportConnections.end()) throw ConnectionException("Transport connection to remote host not found");
		t = e1->second;
	}

	t->AsyncSendMessage(m,handler);
}


void QuicTransport::StartServer(uint16_t port)
{

#ifndef ROBOTRACONTEUR_ALLOW_TCP_LISTEN_PORT_48653
	if (port==48653) throw InvalidArgumentException("Port 48653 is reserved");
#endif
	
	boost::mutex::scoped_lock lock(quic_server_lock);
	if (quic_server)
	{
		throw InvalidOperationException("QUIC server already started");
	}

	quic_server = RR_MAKE_SHARED<detail::PicoQuicServer>(shared_from_this());
    quic_server->StartServer(boost::numeric_cast<uint16_t>(port));
}



int32_t QuicTransport::GetDefaultHeartbeatPeriod()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return heartbeat_period;
}

void QuicTransport::SetDefaultHeartbeatPeriod(int32_t milliseconds)
{
	if (!(milliseconds>0)) throw InvalidArgumentException("Heartbeat must be positive");
	boost::mutex::scoped_lock lock(parameter_lock);
	heartbeat_period=milliseconds;
}

int32_t QuicTransport::GetDefaultReceiveTimeout()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return default_receive_timeout;
}
void QuicTransport::SetDefaultReceiveTimeout(int32_t milliseconds)
{
	if (!(milliseconds>0)) throw InvalidArgumentException("Timeout must be positive");
	boost::mutex::scoped_lock lock(parameter_lock);
	default_receive_timeout=milliseconds;
}
int32_t QuicTransport::GetDefaultConnectTimeout()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return default_connect_timeout;
}
void QuicTransport::SetDefaultConnectTimeout(int32_t milliseconds)
{
	if (!(milliseconds>0)) throw InvalidArgumentException("Timeout must be positive");
	boost::mutex::scoped_lock lock(parameter_lock);
	default_connect_timeout=milliseconds;
}

int32_t QuicTransport::GetMaxMessageSize()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return max_message_size;
}

void QuicTransport::SetMaxMessageSize(int32_t size)
{
	if (size < 16 * 1024 || size > 12 * 1024 * 1024) throw InvalidArgumentException("Invalid maximum message size");
	boost::mutex::scoped_lock lock(parameter_lock);
	max_message_size = size;
}

int32_t QuicTransport::GetMaxConnectionCount()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return max_connection_count;
}

void QuicTransport::SetMaxConnectionCount(int32_t count)
{
	if (count < -1) throw InvalidArgumentException("Invalid maximum connection count");
	boost::mutex::scoped_lock lock(parameter_lock);
	max_connection_count = count;	
}



void QuicTransport_connected_callback2(RR_SHARED_PTR<QuicTransport> parent,RR_SHARED_PTR<detail::PicoQuicConnection> socket, RR_SHARED_PTR<ITransportConnection> connection, RR_SHARED_PTR<RobotRaconteurException> err)
{
	//This is just an empty method.  The connected transport will register when it has a local endpoint.
	
}

void QuicTransport_attach_transport(RR_SHARED_PTR<QuicTransport> parent, RR_SHARED_PTR<detail::PicoQuicConnection> socket,  boost::string_ref url, bool server, uint32_t endpoint, boost::function<void( RR_SHARED_PTR<detail::PicoQuicConnection> , RR_SHARED_PTR<ITransportConnection> , RR_SHARED_PTR<RobotRaconteurException> )>& callback)
{
	try
	{
		RR_SHARED_PTR<QuicTransportConnection> t=RR_MAKE_SHARED<QuicTransportConnection>(parent,url,server,endpoint);
		boost::function<void(RR_SHARED_PTR<RobotRaconteurException>)> h = boost::bind(callback, socket, t, _1);
		t->AsyncAttachSocket(socket,h);
		parent->AddCloseListener(t, &QuicTransportConnection::Close);
	}
	catch (std::exception& )
	{
		RobotRaconteurNode::TryPostToThreadPool(parent->GetNode(),boost::bind(callback,RR_SHARED_PTR<detail::PicoQuicConnection>(), RR_SHARED_PTR<QuicTransportConnection>(),RR_MAKE_SHARED<ConnectionException>("Could not connect to service")),true);
	}

}

void QuicTransport::register_transport(RR_SHARED_PTR<ITransportConnection> connection)
{
	boost::mutex::scoped_lock lock(TransportConnections_lock);
	TransportConnections.insert(std::make_pair(connection->GetLocalEndpoint(),connection));
	//RR_WEAK_PTR<ITransportConnection> w=connection;
	//std::remove(incoming_TransportConnections.begin(), incoming_TransportConnections.end(), w);
}

void QuicTransport::incoming_transport(RR_SHARED_PTR<ITransportConnection> connection)
{
	boost::mutex::scoped_lock lock(TransportConnections_lock);
	//incoming_TransportConnections.push_back(connection);
}

void QuicTransport::erase_transport(RR_SHARED_PTR<ITransportConnection> connection)
{
	try
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		if (TransportConnections.count(connection->GetLocalEndpoint())!=0)
		{
			if (TransportConnections.at(connection->GetLocalEndpoint())==connection)
			{
			
				TransportConnections.erase(connection->GetLocalEndpoint());
			}
		}
	}
	catch (std::exception&) {}

	int32_t connection_count = 0;
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		connection_count = boost::numeric_cast<int32_t>(TransportConnections.size());
	}

	int32_t max_connection_count = GetMaxConnectionCount();

	boost::mutex::scoped_lock lock(quic_server_lock);
	if (max_connection_count > 0)
	{
		if (connection_count < max_connection_count)
		{
			// TODO: restart receiveng connections

		}

	}

	TransportConnectionClosed(connection->GetLocalEndpoint());
}


void QuicTransport::MessageReceived(RR_INTRUSIVE_PTR<Message> m)
{
	GetNode()->MessageReceived(m);
}

bool QuicTransport::GetDisableMessage3()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return disable_message3;
}
void QuicTransport::SetDisableMessage3(bool d)
{
	boost::mutex::scoped_lock lock(parameter_lock);
	disable_message3 = d;
}

bool QuicTransport::GetDisableStringTable()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return disable_string_table;
}
void QuicTransport::SetDisableStringTable(bool d) 
{
	boost::mutex::scoped_lock lock(parameter_lock);
	disable_string_table = d;
}

bool QuicTransport::GetDisableAsyncMessageIO()
{
	boost::mutex::scoped_lock lock(parameter_lock);
	return disable_async_message_io;
}
void QuicTransport::SetDisableAsyncMessageIO(bool d)
{
	boost::mutex::scoped_lock lock(parameter_lock);
	disable_async_message_io = d;
}

QuicTransportConnection::QuicTransportConnection(RR_SHARED_PTR<QuicTransport> parent, boost::string_ref url, bool server, uint32_t local_endpoint) : ASIOStreamBaseTransport(parent->GetNode())
{
	this->parent=parent;
	this->server=server;
	this->m_LocalEndpoint=local_endpoint;
	this->m_RemoteEndpoint=0;
	this->ReceiveTimeout=parent->GetDefaultReceiveTimeout();
	this->HeartbeatPeriod=parent->GetDefaultHeartbeatPeriod();
	this->disable_message3 = parent->GetDisableMessage3();
	this->disable_string_table = parent->GetDisableStringTable();
	this->disable_async_io = parent->GetDisableAsyncMessageIO();
	this->url = RR_MOVE(url.to_string());
	closing = false;

}


void QuicTransportConnection::AsyncAttachSocket(RR_SHARED_PTR<detail::PicoQuicConnection> socket, boost::function<void(RR_SHARED_PTR<RobotRaconteurException>)>& callback)
{
	this->socket=socket;

	int send_timeout=15000;
	

	std::string noden;
	if (!server)
	{
		ParseConnectionURLResult url_res = ParseConnectionURL(url);
		
		target_nodeid = url_res.nodeid;
		
		target_nodename = url_res.nodename;
		if (!(url_res.nodeid.IsAnyNode() && url_res.nodename != ""))
		{
			noden = url_res.nodeid.ToString();
		}
		else
		{
			noden = url_res.nodename;
		}
		
	}
	
	if (!server)
	{
		ASIOStreamBaseTransport::AsyncAttachStream(server,target_nodeid, target_nodename, callback);
		return;
	}
	else
	{
		ASIOStreamBaseTransport::AsyncAttachStream(server,target_nodeid, target_nodename, callback);
	}
	
}

void QuicTransportConnection::MessageReceived(RR_INTRUSIVE_PTR<Message> m)
{
	NodeID RemoteNodeID1;
	uint32_t local_ep;
	uint32_t remote_ep;
	{
		boost::shared_lock<boost::shared_mutex> lock(RemoteNodeID_lock);
		RemoteNodeID1=RemoteNodeID;
		local_ep=m_LocalEndpoint;
		remote_ep=m_RemoteEndpoint;
	}


	//TODO: Finish the endpoint checking procedure

	RR_SHARED_PTR<QuicTransport> p=parent.lock();
	if (!p) return;

	RR_INTRUSIVE_PTR<Message> ret = p->SpecialRequest(m, shared_from_this());
	if (ret != 0)
	{
		try
		{
			if ((m->entries.at(0)->EntryType == MessageEntryType_ConnectionTest || m->entries.at(0)->EntryType == MessageEntryType_ConnectionTestRet))
			{
				if (m->entries.at(0)->Error != MessageErrorType_None)
				{
					Close();
					return;
				}
			}

			if ((ret->entries.at(0)->EntryType == MessageEntryType_ConnectClientRet || ret->entries.at(0)->EntryType == MessageEntryType_ReconnectClient || ret->entries.at(0)->EntryType == MessageEntryType_ConnectClientCombinedRet) && ret->entries.at(0)->Error == MessageErrorType_None)
			{
				if (ret->entries.at(0)->Error == MessageErrorType_None)
				{
					if (ret->header->SenderNodeID == GetNode()->NodeID())
					{
						{
							boost::unique_lock<boost::shared_mutex> lock(RemoteNodeID_lock);
							if (m_LocalEndpoint != 0)
							{
								throw InvalidOperationException("Already connected");
							}

							m_RemoteEndpoint = ret->header->ReceiverEndpoint;
							m_LocalEndpoint = ret->header->SenderEndpoint;
						}


						p->register_transport(RR_STATIC_POINTER_CAST<QuicTransportConnection>(shared_from_this()));
					}
				}
					
			}
			
			boost::function<void(RR_SHARED_PTR<RobotRaconteurException>)> h = boost::bind(&QuicTransportConnection::SimpleAsyncEndSendMessage, RR_STATIC_POINTER_CAST<QuicTransportConnection>(shared_from_this()), _1);
			AsyncSendMessage(ret, h);
		}
		catch (std::exception&)
		{
			Close();
		}

		return;
	}
	
	
	try
	{
		
		if (m->entries.size()==1)
		{
			if ((m->entries[0]->EntryType == MessageEntryType_ConnectClientRet || m->entries[0]->EntryType == MessageEntryType_ConnectClientCombinedRet) && remote_ep==0)
			{
				boost::unique_lock<boost::shared_mutex> lock(RemoteNodeID_lock);
				if (m_RemoteEndpoint == 0)
				{
					m_RemoteEndpoint = m->header->SenderEndpoint;
				}
				remote_ep = m_RemoteEndpoint;
			}
		}

		//We shouldn't get here without having a service connection
		

		boost::asio::ip::address addr=socket->local_endpoint().address();
		uint16_t port=socket->local_endpoint().port();

		std::string scheme="rr+quic";


		std::string connecturl;
		if (addr.is_v4())
		{
			connecturl=scheme + "://" + addr.to_string() + ":" + boost::lexical_cast<std::string>(port) + "/";
		}
		else
		{
			boost::asio::ip::address_v6 addr2=addr.to_v6();
			addr2.scope_id(0);
			/*if (addr2.is_v4_mapped())
			{
				connecturl = scheme + "://" + addr2.to_v4() + ":" + boost::lexical_cast<std::string>(port)+"/";
			}
			else
			{*/
				connecturl = scheme + "://[" + addr2.to_string() + "]:" + boost::lexical_cast<std::string>(port)+"/";
			//}
		}

		Transport::m_CurrentThreadTransportConnectionURL.reset(new std::string(connecturl));
		Transport::m_CurrentThreadTransport.reset(  new RR_SHARED_PTR<ITransportConnection>(RR_STATIC_POINTER_CAST<QuicTransportConnection>(shared_from_this())));
		p->MessageReceived(m);
	}
	catch (std::exception& exp)
	{
		RobotRaconteurNode::TryHandleException(node, &exp);
		Close();
	}

		
	Transport::m_CurrentThreadTransportConnectionURL.reset(0);
	Transport::m_CurrentThreadTransport.reset(0);	
}


void QuicTransportConnection::StreamOpMessageReceived(RR_INTRUSIVE_PTR<Message> m)
{
	return ASIOStreamBaseTransport::StreamOpMessageReceived(m);
}

void QuicTransportConnection::async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler)
{
	boost::mutex::scoped_lock lock(socket_lock);

	
	RobotRaconteurNode::asio_async_write_some(node, socket, b, handler);
}


void QuicTransportConnection::async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler)
{
	boost::mutex::scoped_lock lock(socket_lock);
	
	
	RobotRaconteurNode::asio_async_read_some(node, socket, b, handler);
}

size_t QuicTransportConnection::available()
{
	boost::mutex::scoped_lock lock(socket_lock);

    //TODO: Can we get this from QUIC?
	return 0;
}

bool QuicTransportConnection::IsLargeTransferAuthorized()
{
	boost::shared_lock<boost::shared_mutex> lock2(RemoteNodeID_lock);
	if (m_RemoteEndpoint == 0 || m_LocalEndpoint == 0) return false;
	return GetNode()->IsEndpointLargeTransferAuthorized(m_LocalEndpoint);
}

void QuicTransportConnection::Close()
{
	boost::recursive_mutex::scoped_lock lock(close_lock);
	if(closing) return;
	closing=true;
	{
		boost::mutex::scoped_lock lock(socket_lock);

		
		try
		{
		if (socket->is_open())
		{
		socket->close();
		}
	
		}
		catch (std::exception&) {}


	}

	try
	{
		RR_SHARED_PTR<QuicTransport> p=parent.lock();
		if (p) p->erase_transport(RR_STATIC_POINTER_CAST<QuicTransportConnection>(shared_from_this()));
	}
	catch (std::exception&) {}

	

	ASIOStreamBaseTransport::Close();

}

void QuicTransportConnection::Close1(const boost::system::error_code& ec)
{
	ForceClose();
}

bool QuicTransportConnection::IsClosed()
{
	boost::mutex::scoped_lock lock(socket_lock);

    return socket->is_open();
}

void QuicTransportConnection::ForceClose()
{
	boost::mutex::scoped_lock lock(socket_lock);

    try
	{
		socket->close();
	}
	catch (std::exception&) {}

}

void QuicTransport::CheckConnection(uint32_t endpoint)
{	
	RR_SHARED_PTR<ITransportConnection> t;
	{
		boost::mutex::scoped_lock lock(TransportConnections_lock);
		RR_UNORDERED_MAP<uint32_t, RR_SHARED_PTR<ITransportConnection> >::iterator e1 = TransportConnections.find(endpoint);
		if (e1 == TransportConnections.end()) throw ConnectionException("Transport connection to remote host not found");
		t = e1->second;
	}
	t->CheckConnection(endpoint);
}

uint32_t QuicTransportConnection::GetLocalEndpoint() 
{
	boost::shared_lock<boost::shared_mutex> lock(RemoteNodeID_lock);
	return m_LocalEndpoint;
}

uint32_t QuicTransportConnection::GetRemoteEndpoint()
{
	boost::shared_lock<boost::shared_mutex> lock(RemoteNodeID_lock);
	return m_RemoteEndpoint;
}

void QuicTransportConnection::CheckConnection(uint32_t endpoint)
{
	if (endpoint!=m_LocalEndpoint || !connected.load()) throw ConnectionException("Connection lost");
}

namespace detail
{

	//PicoQuicConnection

	PicoQuicConnection::PicoQuicConnection()
	{
		recv_failed = false;
		can_send = true;
		close_requested = false;
		close_fin_sent = false;
	}

	void PicoQuicConnection::async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler)
	{
		boost::mutex::scoped_lock lock(send_recv_lock);
		if (!can_send)
		{
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(handler,boost::asio::error::broken_pipe,0));
			return;
		}
		
		if (boost::asio::buffer_size(send_buffer) > 0)
		{
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(handler,boost::asio::error::already_started,0));
			return;
		}

		send_buffer = b;
		send_buffer_size = boost::asio::buffer_size(send_buffer);
		send_handler = handler;

		wake_quic();
	}

	void PicoQuicConnection::async_read_some(mutable_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler)
	{
		boost::mutex::scoped_lock lock(send_recv_lock);

		if (recv_handler)
		{
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(handler,boost::asio::error::already_started,0));
			return;
		}

		if (early_recv_data.empty())
		{			
			recv_buffer = b;
			recv_handler = handler;
			return;
		}

		boost::asio::const_buffer& front_early = early_recv_data.front().get<0>();
		size_t read_request_len = boost::asio::buffer_size(b);
		size_t bytes_read = boost::asio::buffer_copy(b, front_early);
		if (bytes_read >= boost::asio::buffer_size(front_early))
		{
			early_recv_data.pop_front();
		} 
		else
		{
			front_early = front_early + bytes_read;
		}

		boost::system::error_code ec;
		RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(handler,ec,bytes_read));
	}

	void PicoQuicConnection::close()
	{
		boost::mutex::scoped_lock lock(send_recv_lock);
		close_requested=true;
		wake_quic();
	}


	int PicoQuicConnection::do_picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    size_t length, picoquic_call_back_event_t fin_or_event)
    {
		switch (fin_or_event)
		{
			case picoquic_callback_stream_data:
			case picoquic_callback_stream_fin:
			{
				bool fin = fin_or_event == picoquic_callback_stream_fin;
				
				if (!recv_failed)
				{
					return do_recv_stream_data(stream_id, bytes, length, fin);
				}
									
				if (fin && stream_id == 0)
				{
					//picoquic_close(cnx,0);
					return 0;
				}
				return -1;
			}
			case picoquic_callback_stream_reset:
				return do_stream_reset(stream_id);
			case picoquic_callback_stop_sending:
				return do_stream_stop_sending(stream_id);

			// Close operation
			case picoquic_callback_stateless_reset:
			case picoquic_callback_close:			
			{				
				{
					boost::mutex::scoped_lock lock(send_recv_lock);
					can_send=false;
				}

				return 0;
			}
			// How to handle cleanly?
			case picoquic_callback_application_close:
				return 0;
			case picoquic_callback_stream_gap:
				// Don't support stream_gap!
				return -1;
			case picoquic_callback_prepare_to_send:
				return do_send_stream_data(stream_id, bytes, length);
			case picoquic_callback_almost_ready:
				return 0;				
			case picoquic_callback_ready:
				// Don't care for server
				return do_ready();		
			case picoquic_callback_datagram:
				// Don't support datagram
				return -1;
			case picoquic_callback_version_negotiation:
				// Not currently supported by quic?
				return -1;
			default:
				// unknown callback
				return -1;
		}        
    }

	int PicoQuicConnection::do_recv_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length, bool fin)
	{
		boost::mutex::scoped_lock lock(send_recv_lock);

		if (stream_id != 0)
		{
			return -1;
		}

		if (recv_handler)
		{
			size_t recv_buffer_size = boost::asio::buffer_size(recv_buffer);
			boost::asio::const_buffers_1 incoming = boost::asio::const_buffers_1(bytes,length);
			
			size_t bytes_copied = boost::asio::buffer_copy(recv_buffer, incoming);
			if (bytes_copied < length)
			{
				boost::asio::const_buffer remaining = incoming + bytes_copied;
				size_t remaining_len = boost::asio::buffer_size(remaining);
				boost::shared_array<uint8_t> remaining_sp(new uint8_t[remaining_len]);
				boost::asio::buffer_copy(boost::asio::buffer(remaining_sp.get(),remaining_len),remaining);
				early_recv_data.push_back(boost::make_tuple(boost::asio::buffer(remaining_sp.get(),remaining_len),remaining_sp));
			} 

			boost::function<void(boost::system::error_code,size_t)> recv_handler1 = RR_MOVE(recv_handler);
			recv_handler.clear();
			boost::system::error_code ec;
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(recv_handler1,ec,bytes_copied));			
		}
		else
		{
			boost::asio::const_buffer remaining = boost::asio::buffer(bytes,length);			
			boost::shared_array<uint8_t> remaining_sp(new uint8_t[length]);
			boost::asio::buffer_copy(boost::asio::buffer(remaining_sp.get(),length),remaining);
			early_recv_data.push_back(boost::make_tuple(boost::asio::buffer(remaining_sp.get(),length),remaining_sp));
		}

		return 0;
	}


	int PicoQuicConnection::do_send_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length)
	{
		boost::mutex::scoped_lock lock(send_recv_lock);
		
		size_t send_size = boost::asio::buffer_size(send_buffer);

		if (stream_id != 0)
		{
			return -1;
		}

		if (close_requested)
		{
			return 0;
		}


		int fin = 0;
		int is_still_active = 0;
		if (send_size > length)
		{
			send_size = length;
			fin = 0;
			is_still_active = 1;
		}

		uint8_t* out_buf = picoquic_provide_stream_data_buffer((void*)bytes, send_size, fin, is_still_active);
		boost::asio::buffer_copy(boost::asio::buffer(out_buf,send_size),send_buffer);
		if (is_still_active == 0)
		{
			send_buffer.clear();
			boost::function<void(boost::system::error_code,size_t)> send_handler1 = RR_MOVE(send_handler);
			send_handler.clear();			
			boost::system::error_code ec;
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(send_handler1,ec,send_buffer_size));
		}
		else
		{
			buffers_consume(send_buffer, send_size);
		}

		return 0;
	}

	int PicoQuicConnection::do_stream_reset(uint64_t stream_id)
	{
		return 0;
	}

	int PicoQuicConnection::do_stream_stop_sending(uint64_t stream)
	{
		return 0;
	}

	int PicoQuicConnection::do_ready()
	{
		return 0;
	}

	void PicoQuicConnection::connection_closed()
	{
		if (send_handler)
		{
			boost::function<void(boost::system::error_code,size_t)> send_handler1 = send_handler;
			send_handler.clear();
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(send_handler1,boost::asio::error::broken_pipe,0));			
		}

		if (send_handler)
		{
			boost::function<void(boost::system::error_code,size_t)> send_handler1 = send_handler;
			send_handler.clear();
			RobotRaconteurNode::TryPostToThreadPool(node, boost::bind(send_handler1,boost::asio::error::broken_pipe,0));			
		}

        std::cerr << "Connection closed" << std::endl;
	}

	//PicoQuicServer

	PicoQuicServer::PicoQuicServer(RR_SHARED_PTR<QuicTransport> parent)
	{
		port = 0;
		keep_going = false;
		this->parent = parent;
		this->node = parent->node;
	}

	void PicoQuicServer::StartServer(uint16_t port)
	{
		boost::mutex::scoped_lock lock(quic_lock);

		if (keep_going)
		{
			throw InvalidOperationException("QUIC Server already started");
		}

		if (port == 0)
		{
			throw InvalidArgumentException("QUIC Port cannot be zero");
		}

		this->port = port;

		boost::asio::ip::udp::endpoint ep_v4(boost::asio::ip::address_v4::any(),port);
        boost::asio::ip::udp::endpoint ep_v6(boost::asio::ip::address_v6::any(),port);
        try
        {
             
            if (picoquic_open_server_sockets(&server_sockets,port))
            {
                throw SystemResourceException("Could not open QUIC server sockets, port in use?");
            }
            v4_socket_.reset(new boost::asio::ip::udp::socket(quic_io_context,boost::asio::ip::udp::v4(),server_sockets.s_socket[1]));
            v6_socket_.reset(new boost::asio::ip::udp::socket(quic_io_context,boost::asio::ip::udp::v6(),server_sockets.s_socket[0]));            
        }
        catch (std::exception)
        {
            v4_socket_.reset();
            v6_socket_.reset();
            throw;
        }

		uint64_t current_time = picoquic_current_time();
        keep_going = true;

		//TODO: don't hard code cert paths
		picoquic_quic_t* qserver1 = picoquic_create(64, "C:\\Users\\wasonj\\libs\\picoquic\\picoquic\\certs\\cert.pem" /*pem_cert*/, 
        "C:\\Users\\wasonj\\libs\\picoquic\\picoquic\\certs\\key.pem" /*pem_key*/, NULL, 
          "experimental.quic.robotraconteur.robotraconteur.com",          
            &PicoQuicServer::picoquic_stream_data_cb, this,
            NULL, NULL, NULL, 
            current_time, NULL, NULL, NULL, 0);

        if (!qserver1)
        {
            throw std::runtime_error("Could not initialize QUIC server context");
        }

        qserver.reset(qserver1, &picoquic_free);

        picoquic_set_default_congestion_algorithm(qserver.get(), picoquic_cubic_algorithm);

        boost::posix_time::time_duration first_timeout = boost::posix_time::seconds(1000);
        quic_timer_.reset(new boost::asio::deadline_timer(quic_io_context,first_timeout));

        begin_v4_wait();
		begin_v6_wait();

        quic_io_context.post(boost::bind(&PicoQuicServer::do_quic,shared_from_this()));

		quic_thread = boost::thread(boost::bind(&PicoQuicServer::run_loop,shared_from_this()));
	}

	uint16_t PicoQuicServer::GetPort()
	{
		boost::mutex::scoped_lock lock(quic_lock);
		if (!keep_going)
		{
			throw InvalidOperationException("QUIC Server not started");
		}
		return port;
	}

	void PicoQuicServer::Close()
	{
		{
			boost::mutex::scoped_lock lock(quic_lock);
			if (!keep_going)
			{
				return;
			}

			keep_going = false;
			port = 0;

			wake_quic();
		}

		quic_thread.join();

		v4_socket_.reset();
		v6_socket_.reset();
		quic_timer_.reset();

		qserver.reset();
	}

	void PicoQuicServer::run_loop()
	{
		while(keep_going)
		{
			quic_io_context.run_one();
		}
	}

	int PicoQuicServer::picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx)
    {
		PicoQuicServer* self = static_cast<PicoQuicServer*>(callback_ctx);

		if (callback_ctx != picoquic_get_default_callback_context(cnx->quic))
		{
			// This should never happen
			return -1;
		}

		switch (fin_or_event)
		{
		case picoquic_callback_almost_ready:
		{
			RR_SHARED_PTR<QuicTransport> parent = self->parent.lock();
			if (!parent)
			{
				return -1;
			}
			RR_SHARED_PTR<PicoQuicServerConnection> c = RR_MAKE_SHARED<PicoQuicServerConnection>(self->shared_from_this(),cnx);
			RR_SHARED_PTR<PicoQuicServerConnection>* c1 = new RR_SHARED_PTR<PicoQuicServerConnection>(c);
			picoquic_set_callback(cnx, &PicoQuicServerConnection::picoquic_stream_data_cb, c1);
			int res = c->picoquic_stream_data_cb(cnx, stream_id, bytes, length, fin_or_event, c1, NULL);

			if (res == 0)
			{
				try
				{					
					boost::function<void(RR_SHARED_PTR<PicoQuicConnection>, RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>)> h
						= boost::bind(&QuicTransport_connected_callback2, parent, _1, _2, _3);
					QuicTransport_attach_transport(parent,c,"",true,0,h);
				}
				catch (std::exception& exp) 
				{
					RobotRaconteurNode::TryHandleException(parent->node, &exp);
					return -1;
				}
			}

			return res;
		}
		default:
			// Should never get here
			return -1;
		}		

        return 0;
    }

    void PicoQuicServer::handle_socket_failure(const boost::system::error_code& ec)
    {
        std::cout << "Socket failure! " << ec << std::endl;
    }

    void PicoQuicServer::handle_quic_failure(int ret)
    {
        std::cout << "QUIC failure! " << ret << std::endl;
	}

    void PicoQuicServer::begin_v4_wait()
    { 
        this->v4_socket_->async_receive(boost::asio::null_buffers(), bind(&PicoQuicServer::handle_v4_wait, shared_from_this(), boost::asio::placeholders::error));
    }

    void PicoQuicServer::handle_v4_wait(boost::system::error_code ec)
    {
        if (ec)
        {
            handle_socket_failure(ec);
            return;
        }
        
        do_quic();

        begin_v4_wait();
    }

	void PicoQuicServer::begin_v6_wait()
    { 
        this->v6_socket_->async_receive(boost::asio::null_buffers(), bind(&PicoQuicServer::handle_v6_wait, shared_from_this(), boost::asio::placeholders::error));
    }

    void PicoQuicServer::handle_v6_wait(boost::system::error_code ec)
    {
        if (ec)
        {
            handle_socket_failure(ec);
            return;
        }
        
        do_quic();

        begin_v6_wait();
    }

    void PicoQuicServer::quic_timer_handler(boost::system::error_code ec)
    {
        if (ec)
        {
            return;
        }

        do_quic();
    }

	void PicoQuicServer::do_quic()
    {        

		boost::mutex::scoped_lock lock(quic_lock);
        quic_timer_->cancel();

        FILE* F_log = stderr;
        int64_t delay_max = 10000000;
        uint64_t current_time = picoquic_current_time();
        int64_t delta_t;
        uint64_t time_before = current_time;
        unsigned char received_ecn;

        socklen_t from_length;
        socklen_t to_length;
        from_length = to_length = sizeof(struct sockaddr_storage);
        unsigned long if_index_to = 0;

        struct sockaddr_storage addr_from;
        struct sockaddr_storage addr_to;

        uint8_t buffer[1536*2];
        int ret=0;

		picoquic_cnx_t* cnx_server = NULL;
        while (keep_going)
        {
            if (ret < 0)
            {
                handle_quic_failure(ret);				
				return;
            }

			bool active_streams = false;
			for (picoquic_cnx_t* cnx = picoquic_get_first_cnx(qserver.get()); cnx != NULL; ) 
			{
				picoquic_cnx_t* cnx_current = cnx;
				cnx = picoquic_get_next_cnx(cnx_current);
				RR_SHARED_PTR<PicoQuicServerConnection>* cnx_context = static_cast<RR_SHARED_PTR<PicoQuicServerConnection>*>(picoquic_get_callback_context(cnx_current));
				if (cnx_context && ((void*)cnx_context) != picoquic_get_default_callback_context(qserver.get()))
				{
					RR_SHARED_PTR<PicoQuicServerConnection> cnx_context1 = *cnx_context;

					if (cnx_context1->close_requested && !cnx_context1->close_fin_sent)
					{
						//picoquic_close(cnx_context1->cnx_client,0);
						cnx_context1->close_fin_sent = true;
						active_streams = true;
					}
					else
					if (cnx_context1->mark_active_streams())
					{
						active_streams = true;
					}
				}					
        	}
			if (active_streams)
			{
				delta_t = 0;
			}
			else
			{
            	delta_t = picoquic_get_next_wake_delay(qserver.get(), current_time, delay_max);
			}

            int bytes_recv = picoquic_select(server_sockets.s_socket, PICOQUIC_NB_SERVER_SOCKETS,
                &addr_from, &from_length,
                &addr_to, &to_length, &if_index_to, &received_ecn,
                buffer, sizeof(buffer),
                0, &current_time);

            if (bytes_recv == 0 && delta_t > 0)
            {
                quic_timer_->expires_from_now(boost::posix_time::milliseconds(delta_t));
                quic_timer_->async_wait(bind(&PicoQuicServer::quic_timer_handler, shared_from_this(), boost::asio::placeholders::error));
                return;
            }

            if (bytes_recv < 0 && !active_streams) {
				int wsa_error = WSAGetLastError();		
                ret = -1;
            } else {
                uint64_t loop_time;

                if (bytes_recv > 0) {
                    /* Submit the packet to the server */
                    ret = picoquic_incoming_packet(qserver.get(), buffer,
                        (size_t)bytes_recv, (struct sockaddr*)&addr_from,
                        (struct sockaddr*)&addr_to, if_index_to, received_ecn,
                        current_time);

                    if (ret != 0) {
                        ret = 0;
                    }
                }
                loop_time = current_time;

				size_t loop_count = 0;
                picoquic_stateless_packet_t* sp;
                int dest_if = -1;
                while ((sp = picoquic_dequeue_stateless_packet(qserver.get())) != NULL) {
                    (void)picoquic_send_through_server_sockets(&server_sockets,
                        (struct sockaddr*)&sp->addr_to,
                        (sp->addr_to.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                        (struct sockaddr*)&sp->addr_local,
                        (sp->addr_local.ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6),
                        dest_if == -1 ? sp->if_index_local : dest_if,
                        (const char*)sp->bytes, (int)sp->length);

                    /* TODO: log stateless packet */
                    if (F_log != NULL) {
                        fflush(F_log);
                    }

                    picoquic_delete_stateless_packet(sp);
					loop_count++;
					if (loop_count > 32) break;
                }

				loop_count = 0;
                picoquic_cnx_t* cnx_next = NULL;
                while (ret == 0 && (cnx_next = picoquic_get_earliest_cnx_to_wake(qserver.get(), loop_time)) != NULL) {
                    int peer_addr_len = 0;
                    struct sockaddr_storage peer_addr;
                    int local_addr_len = 0;
                    struct sockaddr_storage local_addr;

                    uint8_t send_buffer[1536];
                    size_t send_length = 0;
                    ret = picoquic_prepare_packet(cnx_next, current_time,
                        send_buffer, sizeof(send_buffer), &send_length,
                        &peer_addr, &peer_addr_len, &local_addr, &local_addr_len);

                    if (ret == PICOQUIC_ERROR_DISCONNECTED) {
                        ret = 0;

                        if (F_log != NULL) {
                            fprintf(F_log, "%llx: ", (unsigned long long)picoquic_val64_connection_id(picoquic_get_logging_cnxid(cnx_next)));
                            picoquic_log_time(F_log, cnx_server, picoquic_current_time(), "", " : ");
                            fprintf(F_log, "Closed. Retrans= %d, spurious= %d, max sp gap = %d, max sp delay = %d\n",
                                (int)cnx_next->nb_retransmission_total, (int)cnx_next->nb_spurious,
                                (int)cnx_next->path[0]->max_reorder_gap, (int)cnx_next->path[0]->max_spurious_rtt);
                            fflush(F_log);
                        }

                        if (cnx_next == cnx_server) {
                            cnx_server = NULL;
                        }

						// Clean up socket shared pointer reference stored in picoquic
						RR_SHARED_PTR<PicoQuicServerConnection>* cnx_context = static_cast<RR_SHARED_PTR<PicoQuicServerConnection>*>(picoquic_get_callback_context(cnx_next));
						RR_SHARED_PTR<PicoQuicServerConnection> cnx_context1 = *cnx_context;
						if (cnx_context && ((void*)cnx_context) != picoquic_get_default_callback_context(qserver.get()))
						{
							delete cnx_context;
						}
                        picoquic_delete_cnx(cnx_next);

						cnx_context1->connection_closed();

                        //connection_done = 1;

                        break;
                    }
                    else if (ret == 0) {

                        if (send_length > 0) {
                            if (F_log != NULL && (
                                cnx_next->cnx_state < picoquic_state_server_false_start ||
                                cnx_next->cnx_state >= picoquic_state_disconnecting) &&
                                cnx_next->pkt_ctx[picoquic_packet_context_application].send_sequence < 100) {
                                fprintf(F_log, "%llx: ", (unsigned long long)picoquic_val64_connection_id(picoquic_get_logging_cnxid(cnx_next)));
                                fprintf(F_log, "Connection state = %d\n",
                                    picoquic_get_cnx_state(cnx_next));
                            }

                            (void)picoquic_send_through_server_sockets(&server_sockets,
                                (struct sockaddr *)&peer_addr, peer_addr_len, (struct sockaddr *)&local_addr, local_addr_len,
                                dest_if == -1 ? picoquic_get_local_if_index(cnx_next) : dest_if,
                                (const char*)send_buffer, (int)send_length);
                        }
                    }
                    else {
                        break;
                    }

					loop_count++;
					if (loop_count > 32) break;
                }
            }
        }
    
    }

	void PicoQuicServer::wake_quic()
	{
		quic_io_context.post(boost::bind(&PicoQuicServer::do_quic,shared_from_this()));
	}

	//class PicoQuicServerConnection

	PicoQuicServerConnection::PicoQuicServerConnection(RR_SHARED_PTR<PicoQuicServer> parent, picoquic_cnx_t* cnx)
	{
		this->parent = parent;
		node = parent->node;
		cnx_client = cnx;
	}

	
	void PicoQuicServerConnection::wake_quic()
	{
		RR_SHARED_PTR<PicoQuicServer> s = parent.lock();
		if (!s) return;
		s->wake_quic();
	}

	bool PicoQuicServerConnection::mark_active_streams()
	{
		boost::mutex::scoped_lock lock(send_recv_lock);

		if (boost::asio::buffer_size(send_buffer) > 0 || (close_requested && !close_fin_sent))
		{
			void* cnx_context = picoquic_get_callback_context(cnx_client);
			picoquic_mark_active_stream(cnx_client, 0, 1, cnx_context);
			return true;
		}
		return false;
	}

	bool PicoQuicServerConnection::is_open()
	{
		return can_send;
	}

	boost::asio::ip::udp::endpoint PicoQuicServerConnection::local_endpoint()
	{
		return boost::asio::ip::udp::endpoint();
	}

	int PicoQuicServerConnection::picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx)
    {
		RR_SHARED_PTR<PicoQuicServerConnection> self = *static_cast<RR_SHARED_PTR<PicoQuicServerConnection>*>(callback_ctx);

		return self->do_picoquic_stream_data_cb(cnx, stream_id, bytes, length, fin_or_event);
	}

	//class PicoQuicClientConnection

	PicoQuicClientConnection::PicoQuicClientConnection(RR_SHARED_PTR<RobotRaconteurNode> node)		
	{
		this->node = node;		
		has_sent_first = false;
		connect_started = false;
		keep_going = true;
	}

	void PicoQuicClientConnection::async_write_some(const_buffers& b, boost::function<void (const boost::system::error_code& error, size_t bytes_transferred)>& handler)
	{
		PicoQuicConnection::async_write_some(b,handler);

		has_sent_first = true;
	}

	void PicoQuicClientConnection::close()
	{		
		PicoQuicConnection::close();
		
		quic_thread.join();

	}

	bool PicoQuicClientConnection::is_open()
	{
		return connect_started && !close_requested;
	}

	void PicoQuicClientConnection::async_connect(boost::asio::ip::udp::endpoint server_ep, const std::vector<std::string>& sni, boost::function<void(boost::system::error_code)> handler)
	{
		boost::mutex::scoped_lock lock(quic_lock);
		if (connect_started)
		{
			RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(handler,boost::asio::error::already_started));
		}

		connect_started=true;

		boost::shared_ptr<boost::asio::ip::udp::socket> sock2;
        try
        {
            SOCKET_TYPE sock1;
            if(server_ep.address().is_v4())
            {
                sock1 = picoquic_open_client_socket(AF_INET);
                sock2.reset(new boost::asio::ip::udp::socket(quic_io_context,boost::asio::ip::udp::v4(),sock1));       
            }
            else
            {
                sock1 = picoquic_open_client_socket(AF_INET6);
                sock2.reset(new boost::asio::ip::udp::socket(quic_io_context,boost::asio::ip::udp::v6(),sock1));       
            }
            if (sock1 == INVALID_SOCKET)
            {
                RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(handler,boost::asio::error::invalid_argument));
				return;
            }

                 
        }
        catch (std::exception)
        {
            socket_.reset();
            RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(handler,boost::asio::error::fault));
			return;
        }

        uint64_t current_time = picoquic_current_time();

         picoquic_quic_t* qclient1 = picoquic_create(64, NULL, NULL, NULL,
        "experimental.quic.robotraconteur.robotraconteur.com",        
            NULL, NULL, NULL, NULL, NULL, 
            current_time, NULL, NULL, NULL, 0);

        if (!qclient1)
        {
            RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(handler,boost::asio::error::fault));
			return;
        }

        boost::shared_ptr<picoquic_quic_t> qclient2;
        qclient2.reset(qclient1,  [](picoquic_quic_t* p)
		{
			//picoquic_free(p);
		}
		); //&picoquic_free);

        picoquic_set_default_congestion_algorithm(qclient2.get(), picoquic_cubic_algorithm);
        picoquic_set_null_verifier(qclient2.get());

		std::string sni1;
		if (!sni.empty())
		{
			sni1 = sni.front();
		}

        cnx_client = picoquic_create_cnx(qclient2.get(), picoquic_null_connection_id, picoquic_null_connection_id,
            (struct sockaddr*)server_ep.data(), current_time,
            0, sni1.c_str(), "experimental.quic.robotraconteur.robotraconteur.com", 1);

        cnx_client->local_parameters.initial_max_stream_id_unidir = (((uint64_t)2) << 58);

        picoquic_set_callback(cnx_client, PicoQuicClientConnection::picoquic_stream_data_cb, this);

        int ret = picoquic_start_client_cnx(cnx_client);
        if (ret != 0)
        {
			RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(handler,boost::asio::error::fault));
			return;
        }

        socket_ = sock2;
        qclient = qclient2;

        boost::posix_time::time_duration first_timeout = boost::posix_time::seconds(1000);
        quic_timer_.reset(new boost::asio::deadline_timer(quic_io_context,first_timeout));

        begin_socket_wait();

        quic_io_context.post(boost::bind(&PicoQuicClientConnection::do_quic,shared_from_this()));

		connect_handler = handler;

		quic_thread = boost::thread(boost::bind(&PicoQuicClientConnection::run_loop,shared_from_this()));
    }
	

	boost::asio::ip::udp::endpoint PicoQuicClientConnection::local_endpoint()
	{
		if (!socket_)
		{
			return boost::asio::ip::udp::endpoint();
		}
		else
		{
			return socket_->local_endpoint();
		}
	}
	
    void PicoQuicClientConnection::handle_socket_failure(const boost::system::error_code& ec)
    {
        close();
    }

    void PicoQuicClientConnection::handle_quic_failure(int ret)
    {
        close_requested=true;
		connection_closed();
    }

    void PicoQuicClientConnection::begin_socket_wait()
    { 
        this->socket_->async_receive(boost::asio::null_buffers(), bind(&PicoQuicClientConnection::handle_socket_wait, shared_from_this(), boost::asio::placeholders::error));
    }

    void PicoQuicClientConnection::handle_socket_wait(boost::system::error_code ec)
    {
        if (ec)
        {
            handle_socket_failure(ec);
            return;
        }
        
        do_quic();
        
        if (socket_)
        {
            begin_socket_wait();
        }
    }

    void PicoQuicClientConnection::quic_timer_handler(boost::system::error_code ec)
    {
        if (ec)
        {
            return;
        }

        do_quic();
    }

	void PicoQuicClientConnection::run_loop()
	{
		while(keep_going)
		{
			quic_io_context.run_one();				
		}

		new RR_SHARED_PTR<boost::asio::ip::udp::socket>(socket_);

		socket_.reset();		
		quic_timer_.reset();
		qclient.reset();
	}

	void PicoQuicClientConnection::do_quic()
    {        

		boost::mutex::scoped_lock lock(quic_lock);
        quic_timer_->cancel();

        FILE* F_log = stderr;
        int64_t delay_max = 10000000;
        uint64_t current_time = picoquic_current_time();
        int64_t delta_t;
        uint64_t time_before = current_time;
        unsigned char received_ecn;

        socklen_t from_length;
        socklen_t to_length;
        from_length = to_length = sizeof(struct sockaddr_storage);
        unsigned long if_index_to = 0;

        struct sockaddr_storage addr_from;
        struct sockaddr_storage addr_to;

        uint8_t buffer[1536];
        int ret=0;

        while (true)
        {
            if (ret < 0)
            {
                handle_quic_failure(ret);
                keep_going = false;
				return;
            }

			if (picoquic_get_cnx_state(cnx_client) == picoquic_state_disconnected)
            {
                connection_closed();				
                keep_going = false;
				return;
            }

			bool active_streams = false;

			{
				boost::mutex::scoped_lock lock(send_recv_lock);
				if (close_requested && !close_fin_sent)
				{
					//picoquic_close(cnx_client,0);
					close_fin_sent = true;
					active_streams = true;
				}
				else
				if (boost::asio::buffer_size(send_buffer) > 0)
				{
					picoquic_mark_active_stream(cnx_client, 0, 1, this);
					active_streams = true;
				}
			}

			if (!active_streams)
			{
				delta_t = picoquic_get_next_wake_delay(qclient.get(), current_time, delay_max);
			}
			else
			{
				delta_t = 0;
			}
			

            SOCKET_TYPE s1 = socket_->native_handle();
            int bytes_recv = picoquic_select(&s1, 1,
                &addr_from, &from_length,
                &addr_to, &to_length, &if_index_to, &received_ecn,
                buffer, sizeof(buffer),
                0, &current_time);

            if (bytes_recv == 0 && delta_t > 0)
            {
                quic_timer_->expires_from_now(boost::posix_time::milliseconds(delta_t));
                quic_timer_->async_wait(bind(&PicoQuicClientConnection::quic_timer_handler, shared_from_this(), boost::asio::placeholders::error));
                return;
            }

            if (bytes_recv < 0 && !active_streams) {
                ret = -1;
            } else {
                uint64_t loop_time;

                if (bytes_recv > 0) {
                    /* Submit the packet to the server */
                    ret = picoquic_incoming_packet(qclient.get(), buffer,
                        (size_t)bytes_recv, (struct sockaddr*)&addr_from,
                        (struct sockaddr*)&addr_to, if_index_to, received_ecn,
                        current_time);

                    if (ret != 0) {
                        ret = 0;
                    }
                }
                loop_time = current_time;

                
                if (ret == 0) {
                    int peer_addr_len = 0;
                    struct sockaddr_storage peer_addr;
                    int local_addr_len = 0;
                    struct sockaddr_storage local_addr;

                    uint8_t send_buffer[1536];
                    size_t send_length = 0;
                    ret = picoquic_prepare_packet(cnx_client, current_time,
                        send_buffer, sizeof(send_buffer), &send_length,
                        &peer_addr, &peer_addr_len, &local_addr, &local_addr_len);

                    if (ret == 0 && send_length > 0) {
                        SOCKET_TYPE s = socket_->native_handle();
                        int bytes_sent = sendto(s, (const char*)send_buffer, (int)send_length, 0,
                            (struct sockaddr*)&peer_addr, peer_addr_len);

                        if (bytes_sent <= 0)
                        {
                            fprintf(stdout, "Cannot send packet to server, returns %d\n", bytes_sent);

                            if (F_log != stdout && F_log != stderr && F_log != NULL)
                            {
                                fprintf(F_log, "Cannot send packet to server, returns %d\n", bytes_sent);
                            }
                        }
                    }
                }
            }
        }
    }

	void PicoQuicClientConnection::wake_quic()
	{
		quic_io_context.post(boost::bind(&PicoQuicClientConnection::do_quic,shared_from_this()));
	}

	int PicoQuicClientConnection::do_recv_stream_data(uint64_t stream_id, uint8_t* bytes, size_t length, bool fin)
	{
		if (!has_sent_first)
		{
			// Client must send data first
			return -1;
		}

		return PicoQuicConnection::do_recv_stream_data(stream_id,bytes,length,fin);
	}

	int PicoQuicClientConnection::picoquic_stream_data_cb(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, 
    size_t length, picoquic_call_back_event_t fin_or_event, void* callback_ctx, void * stream_ctx)
    {
		PicoQuicClientConnection* self = static_cast<PicoQuicClientConnection*>(callback_ctx);

		return self->do_picoquic_stream_data_cb(cnx, stream_id, bytes, length, fin_or_event);
	}

	int PicoQuicClientConnection::do_ready()
	{
		if (connect_handler)
		{
			boost::function<void(boost::system::error_code)> connect_handler1 = RR_MOVE(connect_handler);
			connect_handler.clear();
			boost::system::error_code ec;
			RobotRaconteurNode::TryPostToThreadPool(node,boost::bind(connect_handler1,ec));
		}
		return 0;
	}


}

namespace detail
{
	QuicConnector::QuicConnector(RR_SHARED_PTR<QuicTransport> parent)
	{
		this->parent = parent;
		connecting = false;
		active_count = 0;
		socket_connected = false;

		connect_timer.reset(new boost::asio::deadline_timer(parent->GetNode()->GetThreadPool()->get_io_context()));
		node = parent->GetNode();

	}

	void QuicConnector::Connect(std::vector<std::string> url, uint32_t endpoint, boost::function<void(RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > handler)
	{
		this->callback = handler;
		this->endpoint = endpoint;
		this->url = url.at(0);
		
		{
			boost::mutex::scoped_lock lock(this_lock);
			connecting = true;			
			_resolver.reset(new boost::asio::ip::udp::resolver(parent->GetNode()->GetThreadPool()->get_io_context()));
			parent->AddCloseListener(_resolver, &boost::asio::ip::udp::resolver::cancel);
		}

		std::vector<boost::tuple<std::string,std::string> > queries;

		BOOST_FOREACH (std::string& e, url)
		{
			try
			{

				ParseConnectionURLResult url_res = ParseConnectionURL(e);

				if (url_res.scheme != "rr+quic") throw InvalidArgumentException("Invalid transport type for QuicTransport");
				if (url_res.host=="") throw ConnectionException("Invalid host for QUIC transport");
				if (url_res.path != "" && url_res.path != "/") throw ConnectionException("Invalid host for QUIC transport");
				std::string host = url_res.host;
				std::string port = boost::lexical_cast<std::string>(url_res.port);

				boost::trim_left_if(host, boost::is_from_range('[', '['));
				boost::trim_right_if(host, boost::is_from_range(']', ']'));
				queries.push_back(boost::make_tuple(host, port));
			}
			catch (std::exception&)
			{
				if (url.size() == 1) throw;

			}
		}

		if (queries.size() == 0) throw ConnectionException("Could not find route to supplied address");
		
		{
			{
				{
					boost::mutex::scoped_lock lock(this_lock);
					if (!connecting) return;

					connect_timer->expires_from_now(boost::posix_time::milliseconds(parent->GetDefaultConnectTimeout()));
					RobotRaconteurNode::asio_async_wait(node, connect_timer, boost::bind(&QuicConnector::connect_timer_callback, shared_from_this(), boost::asio::placeholders::error));

					parent->AddCloseListener(connect_timer, boost::bind(&boost::asio::deadline_timer::cancel, _1));
				}

				typedef boost::tuple<std::string, std::string> e_type;
				BOOST_FOREACH(e_type& e, queries)
				{
					int32_t key2;
					{
						boost::mutex::scoped_lock lock(this_lock);
						active_count++;
						key2 = active_count;
#if BOOST_ASIO_VERSION < 101200
						boost::asio::ip::basic_resolver_query<boost::asio::ip::udp> q(e.get<0>(), e.get<1>(), boost::asio::ip::resolver_query_base::flags());
						RobotRaconteurNode::asio_async_resolve(node, _resolver, q, boost::bind(&QuicConnector::connect2, shared_from_this(), key2, boost::asio::placeholders::error, boost::asio::placeholders::iterator, callback));
#else
						RobotRaconteurNode::asio_async_resolve(node, _resolver, e.get<0>(), e.get<1>(), boost::bind(&QuicConnector::connect2, shared_from_this(), key2, boost::asio::placeholders::error, boost::asio::placeholders::results, callback));
#endif
						//std::cout << "Begin resolve" << std::endl;
						
						active.push_back(key2);
					}
				}
			}
		}
	}

#if BOOST_ASIO_VERSION < 101200
	void QuicConnector::connect2(int32_t key, const boost::system::error_code& err, boost::asio::ip::udp::resolver::iterator endpoint_iterator, boost::function<void(RR_SHARED_PTR<QuicTransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback)
	{
#else
	void QuicConnector::connect2(int32_t key, const boost::system::error_code& err, boost::asio::ip::udp::resolver::results_type results, boost::function<void(RR_SHARED_PTR<QuicTransportConnection>, RR_SHARED_PTR<RobotRaconteurException>) > callback)
	{
		boost::asio::ip::udp::resolver::results_type::iterator endpoint_iterator = results.begin();
#endif
		
		if (err)
		{
			handle_error(key, err);
			return;
		}
				
		//std::cout << "End resolve" << std::endl;
		try
		{

			std::vector<boost::asio::ip::udp::endpoint> ipv4;
			std::vector<boost::asio::ip::udp::endpoint> ipv6;

			boost::asio::ip::basic_resolver_iterator<boost::asio::ip::udp> end;

			for (; endpoint_iterator != end; endpoint_iterator++)
			{
				if (endpoint_iterator->endpoint().address().is_v4()) ipv4.push_back(endpoint_iterator->endpoint());
				if (endpoint_iterator->endpoint().address().is_v6()) ipv6.push_back(endpoint_iterator->endpoint());
			}

			if (ipv4.size() == 0 && ipv6.size() == 0)
			{
				handle_error(key, boost::system::error_code(boost::system::errc::bad_address, boost::system::generic_category()));
				return;
			}


			BOOST_FOREACH (boost::asio::ip::udp::endpoint& e, ipv4)
			{
				RR_SHARED_PTR<PicoQuicClientConnection> sock(new PicoQuicClientConnection(parent->GetNode()));

				int32_t key2;
				{
					boost::mutex::scoped_lock lock(this_lock);
					if (!connecting) return;

					active_count++;
					key2 = active_count;

					RR_SHARED_PTR<boost::signals2::scoped_connection> sock_closer
						= RR_MAKE_SHARED<boost::signals2::scoped_connection>(
							parent->AddCloseListener(sock, boost::bind(&PicoQuicClientConnection::close, _1))
							);
					
					RobotRaconteurNode::asio_async_connect(node,sock, e, sni, boost::bind(&QuicConnector::connected_callback, shared_from_this(), sock, sock_closer, key2, boost::asio::placeholders::error));
					//std::cout << "Start connect " << e->address() << ":" << e->port() << std::endl;
					
					active.push_back(key2);
				}
				boost::this_thread::sleep(boost::posix_time::milliseconds(5));								
			}


			std::vector<boost::asio::ip::address> local_ip;

			TcpTransport::GetLocalAdapterIPAddresses(local_ip);

			std::vector<uint32_t> scopeids;
			BOOST_FOREACH (boost::asio::ip::address& ee, local_ip)
			{
				if (ee.is_v6())
				{
					boost::asio::ip::address_v6 a6 = ee.to_v6();
					if (a6.is_link_local())
					{
						if (std::find(scopeids.begin(), scopeids.end(), a6.scope_id()) == scopeids.end())
						{
							scopeids.push_back(a6.scope_id());
						}
					}
				}
			}


			BOOST_FOREACH (boost::asio::ip::udp::endpoint& e, ipv6)
			{

				std::vector<boost::asio::ip::udp::endpoint> ipv62;

				if (!e.address().is_v6()) continue;

				boost::asio::ip::address_v6 addr = e.address().to_v6();
				uint16_t port = e.port();

				if (!addr.is_link_local() || (addr.is_link_local() && addr.scope_id() != 0))
				{
					ipv62.push_back(e);
				}
				else
				{
					//Link local address with no scope id, we need to try them all...

					BOOST_FOREACH (uint32_t e3, scopeids)
					{
						boost::asio::ip::address_v6 addr3 = addr;
						addr3.scope_id(e3);
						ipv62.push_back(boost::asio::ip::udp::endpoint(addr3, port));
					}
				}


				BOOST_FOREACH (boost::asio::ip::udp::endpoint& e2, ipv62)
				{

					RR_SHARED_PTR<PicoQuicClientConnection> sock(new PicoQuicClientConnection(parent->GetNode()));

					int32_t key2;
					{
						boost::mutex::scoped_lock lock(this_lock);
						if (!connecting) return;

						active_count++;
						key2 = active_count;
						
						RR_SHARED_PTR<boost::signals2::scoped_connection> sock_closer =
							RR_MAKE_SHARED<boost::signals2::scoped_connection>(
								parent->AddCloseListener(sock, boost::bind(&PicoQuicClientConnection::close, _1))
								);
						
						RobotRaconteurNode::asio_async_connect(node, sock, e2, sni, boost::bind(&QuicConnector::connected_callback, shared_from_this(), sock, sock_closer, key2, boost::asio::placeholders::error));
						
						active.push_back(key2);
					}

					//std::cout << "Start connect [" << e2->address() << "]:" << e2->port() << std::endl;

					boost::this_thread::sleep(boost::posix_time::milliseconds(5));					
				}
			}



			boost::mutex::scoped_lock lock(this_lock);
			active.remove(key);
		}
		catch (std::exception&)
		{
			handle_error(key, boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category()));
			return;
		}


		bool all_stopped = false;
		{
			boost::mutex::scoped_lock lock(this_lock);
			all_stopped = active.size() == 0;
		}

		if (all_stopped)
		{
			boost::mutex::scoped_lock lock(this_lock);
			if (!connecting) return;
			connecting = false;
		}

		if (all_stopped)
		{
			if (errors.size() == 0)
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), RR_MAKE_SHARED<ConnectionException>("Could not connect to remote node"));
				return;
			}

			BOOST_FOREACH (RR_SHARED_PTR<RobotRaconteurException>& e, errors)
			{
				RR_SHARED_PTR<NodeNotFoundException> e2 = RR_DYNAMIC_POINTER_CAST<NodeNotFoundException>(e);
				if (e2)
				{
					callback(RR_SHARED_PTR<QuicTransportConnection>(), e2);
					return;
				}
			}

			BOOST_FOREACH (RR_SHARED_PTR<RobotRaconteurException>& e, errors)
			{
				RR_SHARED_PTR<AuthenticationException> e2 = RR_DYNAMIC_POINTER_CAST<AuthenticationException>(e);
				if (e2)
				{
					callback(RR_SHARED_PTR<QuicTransportConnection>(), e2);
					return;
				}
			}

			callback(RR_SHARED_PTR<QuicTransportConnection>(), errors.back());
		}




	}

	void QuicConnector::connected_callback(RR_SHARED_PTR<PicoQuicConnection> socket, RR_SHARED_PTR<boost::signals2::scoped_connection> socket_closer, int32_t key, const boost::system::error_code& error)
	{
		if (error)
		{
			handle_error(key, error);
			return;
		}
		try
		{
			boost::mutex::scoped_lock lock(this_lock);
			bool c = connecting;
						
			if (!c)
			{
				return;
			}

			int32_t key2;
			{				
				active_count++;
				key2 = active_count;
				socket_connected = true;

				boost::function<void(RR_SHARED_PTR<PicoQuicConnection>, RR_SHARED_PTR<ITransportConnection>, RR_SHARED_PTR<RobotRaconteurException>)> cb = boost::bind(&QuicConnector::connected_callback2, shared_from_this(), _1, key2, _2, _3);
				QuicTransport_attach_transport(parent, socket, url, false, endpoint, cb);
				
				active.push_back(key2);
			}
			
			active.remove(key);
		}
		catch (std::exception&)
		{
			handle_error(key, boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category()));
		}
	}	

	void QuicConnector::connected_callback2(RR_SHARED_PTR<PicoQuicConnection> socket, int32_t key, RR_SHARED_PTR<ITransportConnection> connection, RR_SHARED_PTR<RobotRaconteurException> err)
	{
		if (err)
		{
			if (connection)
			{
				try
				{
					connection->Close();
				}
				catch (std::exception&) {}
			}
			//callback(RR_SHARED_PTR<QuicTransportConnection>(),err);
			handle_error(key, err);
			return;

		}
		try
		{
			{


				bool c;
				{
					boost::mutex::scoped_lock lock(this_lock);
					c = connecting;
					connecting = false;
				}
				if (!c)
				{

					try
					{
						//std::cout << "Closing 2" << std::endl;
						connection->Close();
					}
					catch (std::exception&) {}

					return;
				}

			}
			parent->register_transport(connection);

			{
				boost::mutex::scoped_lock lock(this_lock);
				if (connect_timer) connect_timer->cancel();
				connect_timer.reset();
			}

			try
			{

				//std::cout << "connect callback" << std::endl;
				callback(boost::dynamic_pointer_cast<ITransportConnection>(connection), RR_SHARED_PTR<RobotRaconteurException>());
			}
			catch (std::exception& exp)
			{
				RobotRaconteurNode::TryHandleException(node, &exp);
			}
		}
		catch (std::exception&)
		{
			handle_error(key, boost::system::error_code(boost::system::errc::io_error, boost::system::generic_category()));
		}

	}
	
	void QuicConnector::connect_timer_callback(const boost::system::error_code& e)
	{


		if (e != boost::asio::error::operation_aborted)
		{
			{
				boost::mutex::scoped_lock lock(this_lock);
				if (!connecting) return;
				connecting = false;
			}
			try
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), RR_MAKE_SHARED<ConnectionException>("Connection timed out"));
			}
			catch (std::exception& exp)
			{
				RobotRaconteurNode::TryHandleException(node, &exp);
			}
		}


	}

	void QuicConnector::handle_error(const int32_t& key, const boost::system::error_code& err)
	{
		handle_error(key, RR_MAKE_SHARED<ConnectionException>(err.message()));
	}

	void QuicConnector::handle_error(const int32_t& key, RR_SHARED_PTR<RobotRaconteurException> err)
	{
		bool s;
		bool c;
		{
			boost::mutex::scoped_lock lock(this_lock);
			if (!connecting) return;
			
			active.remove(key);
			errors.push_back(err);

			if (active.size() != 0) return;
			s = socket_connected;
			if (active.size() != 0) return;		

		//return;
		//All activities have completed, assume failure
						
			c = connecting;
			connecting = false;
			
			if (!c) return;

			connect_timer.reset();
		}

		BOOST_FOREACH (RR_SHARED_PTR<RobotRaconteurException> e, errors)
		{
			RR_SHARED_PTR<NodeNotFoundException> e2 = RR_DYNAMIC_POINTER_CAST<NodeNotFoundException>(e);
			if (e2)
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), e2);
				return;
			}
		}

		BOOST_FOREACH (RR_SHARED_PTR<RobotRaconteurException> e, errors)
		{
			RR_SHARED_PTR<AuthenticationException> e2 = RR_DYNAMIC_POINTER_CAST<AuthenticationException>(e);
			if (e2)
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), e2);
				return;
			}
		}

		if (!s)
		{
			try
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), err);
			}
			catch (std::exception& exp)
			{
				RobotRaconteurNode::TryHandleException(node, &exp);
			}
		}
		else
		{
			try
			{
				callback(RR_SHARED_PTR<QuicTransportConnection>(), RR_MAKE_SHARED<ConnectionException>("Could not connect to service"));
			}
			catch (std::exception& exp)
			{
				RobotRaconteurNode::TryHandleException(node, &exp);
			}
		}
	}
}

}
