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

    // TODO: Create connection
	/*RR_SHARED_PTR<detail::TcpConnector> c=RR_MAKE_SHARED<detail::TcpConnector>(shared_from_this());
	std::vector<std::string> url2;
	url2.push_back(url.to_string());
	c->Connect(url2,e->GetLocalEndpoint(),callback);*/
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

}
