#include <RobotRaconteur.h>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <boost/algorithm/string.hpp>

#include "robotraconteur_generated.h"
#include "ServiceTest.h"
#include "ServiceTestClient.h"
#include "ServiceTest2.h"
#include "ServiceTestClient2.h"

#include "RobotRaconteur/QuicTransport.h"

using namespace RobotRaconteur;
using namespace RobotRaconteurTest;
using namespace std;
using namespace com::robotraconteur::testing::TestService1;
using namespace com::robotraconteur::testing::TestService2;
using namespace com::robotraconteur::testing::TestService3;

int main(int argc, char* argv[])
{
    std::string command;
	if (argc > 1) 
		command=std::string(argv[1]);
	else
		command="loopback";

    if (command=="loopback")
	{

		
		RR_SHARED_PTR<QuicTransport> c=RR_MAKE_SHARED<QuicTransport>();
		c->StartServer(64565);
		
        RR_SHARED_PTR<TcpTransport> c2=RR_MAKE_SHARED<TcpTransport>();
		c2->StartServer(4565);

		//c->EnableNodeAnnounce();
		//c->EnableNodeDiscoveryListening();

		RobotRaconteurNode::s()->RegisterTransport(c);
        RobotRaconteurNode::s()->RegisterTransport(c2);
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService1Factory>());
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService2Factory>());

		RobotRaconteurTestServiceSupport s;
		s.RegisterServices(c2);

		int count = 1;

		if (argc >= 3)
		{
			std::string scount(argv[2]);
			count = boost::lexical_cast<int>(scount);
		}
		
		for (int j=0; j<count; j++)
		{
			ServiceTestClient cl;
			cl.RunFullTest("rr+quic://localhost:64565?service=RobotRaconteurTestService", "rr+quic://localhost:64565?service=RobotRaconteurTestService_auth");								
		}

		cout << "start shutdown" << endl;

		RobotRaconteurNode::s()->Shutdown();
		
		cout << "Test completed, no errors detected!" << endl;
        return 0;
    }

    if (command=="minimalloopback")
	{

		
		RR_SHARED_PTR<QuicTransport> c=RR_MAKE_SHARED<QuicTransport>();
		c->StartServer(64565);
		c->SetDisableAsyncMessageIO(true);
		
        RR_SHARED_PTR<TcpTransport> c2=RR_MAKE_SHARED<TcpTransport>();
		c2->StartServer(4565);

		//c->EnableNodeAnnounce();
		//c->EnableNodeDiscoveryListening();

		RobotRaconteurNode::s()->RegisterTransport(c);
        RobotRaconteurNode::s()->RegisterTransport(c2);
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService1Factory>());
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService2Factory>());

		RobotRaconteurTestServiceSupport s;
		s.RegisterServices(c2);

		int count = 1;

		if (argc >= 3)
		{
			std::string scount(argv[2]);
			count = boost::lexical_cast<int>(scount);
		}
		
		for (int j=0; j<count; j++)
		{
			ServiceTestClient cl;
			cl.RunMinimalTest("rr+quic://[::1]:64565?service=RobotRaconteurTestService");								
		}

		cout << "start shutdown" << endl;

		RobotRaconteurNode::s()->Shutdown();
		
		cout << "Test completed, no errors detected!" << endl;

		return 0;
    }

	if (command == "server")
	{
		RR_SHARED_PTR<LocalTransport> c2=RR_MAKE_SHARED<LocalTransport>();
		c2->StartServerAsNodeName("quictest", false);

		RR_SHARED_PTR<TcpTransport> c=RR_MAKE_SHARED<TcpTransport>();		
		
		c->StartServer(4565);
		
		RR_SHARED_PTR<QuicTransport> c3=RR_MAKE_SHARED<QuicTransport>();
		c3->StartServer(64565);		

		RobotRaconteurNode::s()->RegisterTransport(c);
		RobotRaconteurNode::s()->RegisterTransport(c2);		
		RobotRaconteurNode::s()->RegisterTransport(c3);		
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService1Factory>());
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService2Factory>());
		RobotRaconteurNode::s()->RegisterServiceType(RR_MAKE_SHARED<com__robotraconteur__testing__TestService3Factory>());

		RobotRaconteurTestServiceSupport s;
		s.RegisterServices(c);

		RobotRaconteurTestService2Support s2;
		s2.RegisterServices(c);

		cout << "QUIC test server started, press enter to quit" << endl;
		getchar();
		RobotRaconteurNode::s()->Shutdown();
		cout << "Test completed, no errors detected!" << endl;
		return 0;
	}

    throw std::runtime_error("unknown command");

}

