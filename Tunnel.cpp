#include "I2PEndian.h"
#include <thread>
#include <cryptopp/sha.h>
#include "RouterContext.h"
#include "Log.h"
#include "Timestamp.h"
#include "I2NPProtocol.h"
#include "Transports.h"
#include "NetDb.h"
#include "Tunnel.h"

namespace i2p
{	
namespace tunnel
{		
	
	Tunnel::Tunnel (TunnelConfig * config): m_Config (config), m_Pool (nullptr), 
		m_IsEstablished (false), m_IsFailed (false)
	{
	}	

	Tunnel::~Tunnel ()
	{
		delete m_Config;
	}	

	void Tunnel::Build (uint32_t replyMsgID, OutboundTunnel * outboundTunnel)
	{
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		size_t numRecords = m_Config->GetNumHops ();
		I2NPMessage * msg = NewI2NPMessage ();
		*msg->GetPayload () = numRecords;
		msg->len += numRecords*sizeof (I2NPBuildRequestRecordElGamalEncrypted) + 1;
		
		I2NPBuildRequestRecordElGamalEncrypted * records = (I2NPBuildRequestRecordElGamalEncrypted *)(msg->GetPayload () + 1); 

		TunnelHopConfig * hop = m_Config->GetFirstHop ();
		int i = 0;
		while (hop)
		{
			EncryptBuildRequestRecord (*hop->router,
				CreateBuildRequestRecord (hop->router->GetIdentHash (), 
				    hop->tunnelID,
					hop->nextRouter->GetIdentHash (), 
					hop->nextTunnelID,
					hop->layerKey, hop->ivKey,                  
					hop->replyKey, hop->replyIV,
					hop->next ? rnd.GenerateWord32 () : replyMsgID, // we set replyMsgID for last hop only
				    hop->isGateway, hop->isEndpoint), 
		    	records[i]);
			i++;
			hop = hop->next;
		}	

		i2p::crypto::CBCDecryption decryption;
		hop = m_Config->GetLastHop ()->prev;
		size_t ind = numRecords - 1;
		while (hop)
		{
			decryption.SetKey (hop->replyKey);
			decryption.SetIV (hop->replyIV);
			for (size_t i = ind; i < numRecords; i++)	
				decryption.Decrypt((uint8_t *)&records[i], 
					sizeof (I2NPBuildRequestRecordElGamalEncrypted), (uint8_t *)&records[i]);	
			hop = hop->prev;
			ind--;
		}	
		FillI2NPMessageHeader (msg, eI2NPVariableTunnelBuild);
		
		if (outboundTunnel)
			outboundTunnel->SendTunnelDataMsg (GetNextIdentHash (), 0, msg);	
		else
			i2p::transports.SendMessage (GetNextIdentHash (), msg);
	}	
		
	bool Tunnel::HandleTunnelBuildResponse (uint8_t * msg, size_t len)
	{
		LogPrint ("TunnelBuildResponse ", (int)msg[0], " records.");

		i2p::crypto::CBCDecryption decryption;
		TunnelHopConfig * hop = m_Config->GetLastHop (); 
		int num = msg[0];
		while (hop)
		{	
			decryption.SetKey (hop->replyKey);
			decryption.SetIV (hop->replyIV);
			for (int i = 0; i < num; i++)
			{			
				uint8_t * record = msg + 1 + i*sizeof (I2NPBuildResponseRecord);
				decryption.Decrypt(record, sizeof (I2NPBuildResponseRecord), record);
			}
			hop = hop->prev;
			num--;
		}

		m_IsEstablished = true;
		for (int i = 0; i < msg[0]; i++)
		{			
			I2NPBuildResponseRecord * record = (I2NPBuildResponseRecord *)(msg + 1 + i*sizeof (I2NPBuildResponseRecord));
			LogPrint ("Ret code=", (int)record->ret);
			if (record->ret) 
				// if any of participants declined the tunnel is not established
				m_IsEstablished = false; 
		}
		if (m_IsEstablished) 
		{
			// change reply keys to layer keys
			TunnelHopConfig * hop = m_Config->GetFirstHop ();
			while (hop)
			{
				hop->decryption.SetKeys (hop->layerKey, hop->ivKey);
				hop = hop->next;
			}	
		}	
		return m_IsEstablished;
	}	

	void Tunnel::EncryptTunnelMsg (I2NPMessage * tunnelMsg)
	{
		uint8_t * payload = tunnelMsg->GetPayload () + 4;
		TunnelHopConfig * hop = m_Config->GetLastHop (); 
		while (hop)
		{	
			hop->decryption.Decrypt (payload);
			hop = hop->prev;
		}
	}	
	
	void InboundTunnel::HandleTunnelDataMsg (I2NPMessage * msg)
	{
		if (IsFailed ()) SetFailed (false); // incoming messages means a tunnel is alive			
		msg->from = this;
		EncryptTunnelMsg (msg);
		m_Endpoint.HandleDecryptedTunnelDataMsg (msg);	
	}	

	void OutboundTunnel::SendTunnelDataMsg (const uint8_t * gwHash, uint32_t gwTunnel, i2p::I2NPMessage * msg)
	{
		TunnelMessageBlock block;
		if (gwHash)
		{
			block.hash = gwHash;
			if (gwTunnel)
			{	
				block.deliveryType = eDeliveryTypeTunnel;
				block.tunnelID = gwTunnel;
			}	
			else
				block.deliveryType = eDeliveryTypeRouter;
		}	
		else	
			block.deliveryType = eDeliveryTypeLocal;
		block.data = msg;
		
		std::unique_lock<std::mutex> l(m_SendMutex);
		m_Gateway.SendTunnelDataMsg (block);
	}
		
	void OutboundTunnel::SendTunnelDataMsg (std::vector<TunnelMessageBlock> msgs)
	{
		std::unique_lock<std::mutex> l(m_SendMutex);
		for (auto& it : msgs)
			m_Gateway.PutTunnelDataMsg (it);
		m_Gateway.SendBuffer ();
	}	
	
	Tunnels tunnels;
	
	Tunnels::Tunnels (): m_IsRunning (false), m_IsTunnelCreated (false), 
		m_NextReplyMsgID (555), m_Thread (nullptr), m_ExploratoryPool (nullptr)
	{
	}
	
	Tunnels::~Tunnels ()	
	{
		for (auto& it : m_OutboundTunnels)
			delete it;
		m_OutboundTunnels.clear ();

		for (auto& it : m_InboundTunnels)
			delete it.second;
		m_InboundTunnels.clear ();
		
		for (auto& it : m_TransitTunnels)
			delete it.second;
		m_TransitTunnels.clear ();

		for (auto& it : m_PendingTunnels)
			delete it.second;
		m_PendingTunnels.clear ();

		for (auto& it: m_Pools)
			delete it.second;
		m_Pools.clear ();
	}	
	
	InboundTunnel * Tunnels::GetInboundTunnel (uint32_t tunnelID)
	{
		auto it = m_InboundTunnels.find(tunnelID);
		if (it != m_InboundTunnels.end ())
			return it->second;
		return nullptr;
	}	
	
	TransitTunnel * Tunnels::GetTransitTunnel (uint32_t tunnelID)
	{
		auto it = m_TransitTunnels.find(tunnelID);
		if (it != m_TransitTunnels.end ())
			return it->second;
		return nullptr;
	}	
		
	Tunnel * Tunnels::GetPendingTunnel (uint32_t replyMsgID)
	{
		auto it = m_PendingTunnels.find(replyMsgID);
		if (it != m_PendingTunnels.end ())
		{
			Tunnel * t = it->second;
			m_PendingTunnels.erase (it);
			return t;
		}	
		return nullptr;
	}	

	InboundTunnel * Tunnels::GetNextInboundTunnel ()
	{
		InboundTunnel * tunnel  = nullptr; 
		size_t minReceived = 0;
		for (auto it : m_InboundTunnels)
		{
			if (it.second->IsFailed ()) continue;
			if (!tunnel || it.second->GetNumReceivedBytes () < minReceived)
			{
				tunnel = it.second;
				minReceived = it.second->GetNumReceivedBytes ();
			}
		}			
		return tunnel;
	}
	
	OutboundTunnel * Tunnels::GetNextOutboundTunnel ()
	{
		CryptoPP::RandomNumberGenerator& rnd = i2p::context.GetRandomNumberGenerator ();
		uint32_t ind = rnd.GenerateWord32 (0, m_OutboundTunnels.size () - 1), i = 0;
		OutboundTunnel * tunnel = nullptr;
		for (auto it: m_OutboundTunnels)
		{	
			if (i >= ind) return it;
			if (!it->IsFailed ())
			{
				tunnel = it;
				i++;
			}
		}	
		return tunnel;
	}	

	TunnelPool * Tunnels::CreateTunnelPool (i2p::data::LocalDestination& localDestination)
	{
		auto pool = new TunnelPool (localDestination);
		m_Pools[pool->GetIdentHash ()] = pool;
		return pool;
	}	

	void Tunnels::DeleteTunnelPool (TunnelPool * pool)
	{
		if (pool)
		{
			m_Pools.erase (pool->GetIdentHash ());
			delete pool;
		}
	}	
	
	void Tunnels::AddTransitTunnel (TransitTunnel * tunnel)
	{
		m_TransitTunnels[tunnel->GetTunnelID ()] = tunnel;
	}	

	void Tunnels::Start ()
	{
		m_IsRunning = true;
		m_Thread = new std::thread (std::bind (&Tunnels::Run, this));
	}
	
	void Tunnels::Stop ()
	{
		m_IsRunning = false;
		m_Queue.WakeUp ();
		if (m_Thread)
		{	
			m_Thread->join (); 
			delete m_Thread;
			m_Thread = 0;
		}	
	}	

	void Tunnels::Run ()
	{
		std::this_thread::sleep_for (std::chrono::seconds(1)); // wait for other parts are ready
		
		uint64_t lastTs = 0;
		while (m_IsRunning)
		{
			try
			{	
				I2NPMessage * msg = m_Queue.GetNextWithTimeout (1000); // 1 sec
				while (msg)
				{
					uint32_t  tunnelID = be32toh (*(uint32_t *)msg->GetPayload ()); 
					InboundTunnel * tunnel = GetInboundTunnel (tunnelID);
					if (tunnel)
						tunnel->HandleTunnelDataMsg (msg);
					else
					{	
						TransitTunnel * transitTunnel = GetTransitTunnel (tunnelID);
						if (transitTunnel)
							transitTunnel->HandleTunnelDataMsg (msg);
						else	
						{	
							LogPrint ("Tunnel ", tunnelID, " not found");
							i2p::DeleteI2NPMessage (msg);
						}	
					}	
					msg = m_Queue.Get ();
				}	
			
				uint64_t ts = i2p::util::GetSecondsSinceEpoch ();
				if (ts - lastTs >= 15) // manage tunnels every 15 seconds
				{
					ManageTunnels ();
					lastTs = ts;
				}
			}
			catch (std::exception& ex)
			{
				LogPrint ("Tunnels: ", ex.what ());
			}	
		}	
	}	

	void Tunnels::ManageTunnels ()
	{
		// check pending tunnel. if something is still there, wipe it out
		// because it wouldn't be reponded anyway
		for (auto& it : m_PendingTunnels)
		{	
			LogPrint ("Pending tunnel build request ", it.first, " has not been responded. Deleted");
			delete it.second;
		}	
		m_PendingTunnels.clear ();
		
		ManageInboundTunnels ();
		ManageOutboundTunnels ();
		ManageTransitTunnels ();
		ManageTunnelPools ();
		
	/*	if (!m_IsTunnelCreated)
		{	
			InboundTunnel * inboundTunnel = CreateOneHopInboundTestTunnel ();
			if (inboundTunnel)
			    CreateOneHopOutboundTestTunnel (inboundTunnel);
			inboundTunnel = CreateTwoHopsInboundTestTunnel ();
			if (inboundTunnel)
				CreateTwoHopsOutboundTestTunnel (inboundTunnel);
				
			m_IsTunnelCreated = true;
		}*/
	}	

	void Tunnels::ManageOutboundTunnels ()
	{
		uint64_t ts = i2p::util::GetSecondsSinceEpoch ();
		for (auto it = m_OutboundTunnels.begin (); it != m_OutboundTunnels.end ();)
		{
			if (ts > (*it)->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
			{
				LogPrint ("Tunnel ", (*it)->GetTunnelID (), " expired");
				auto pool = (*it)->GetTunnelPool ();
				if (pool)
					pool->TunnelExpired (*it);
				it = m_OutboundTunnels.erase (it);
			}	
			else 
				it++;
		}
	
		if (m_OutboundTunnels.size () < 5) 
		{
			// trying to create one more oubound tunnel
			InboundTunnel * inboundTunnel = GetNextInboundTunnel ();
			if (!inboundTunnel) return;
			LogPrint ("Creating one hop outbound tunnel...");
			CreateTunnel<OutboundTunnel> (
			  	new TunnelConfig (std::vector<const i2p::data::RouterInfo *> 
				    { 
						i2p::data::netdb.GetRandomRouter ()
					},		
		     		inboundTunnel->GetTunnelConfig ()));
		}
	}
	
	void Tunnels::ManageInboundTunnels ()
	{
		uint64_t ts = i2p::util::GetSecondsSinceEpoch ();
		for (auto it = m_InboundTunnels.begin (); it != m_InboundTunnels.end ();)
		{
			if (ts > it->second->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
			{
				LogPrint ("Tunnel ", it->second->GetTunnelID (), " expired");
				auto pool = it->second->GetTunnelPool ();
				if (pool)
					pool->TunnelExpired (it->second);
				it = m_InboundTunnels.erase (it);
			}	
			else 
				it++;
		}

		if (m_InboundTunnels.empty ())
		{
			LogPrint ("Creating zero hops inbound tunnel...");
			CreateZeroHopsInboundTunnel ();
			if (!m_ExploratoryPool)
				m_ExploratoryPool = CreateTunnelPool (i2p::context);
			return;
		}
		
		if (m_OutboundTunnels.empty () || m_InboundTunnels.size () < 5) 
		{
			// trying to create one more inbound tunnel			
			LogPrint ("Creating one hop inbound tunnel...");
			CreateTunnel<InboundTunnel> (
				new TunnelConfig (std::vector<const i2p::data::RouterInfo *>
				    {              
						i2p::data::netdb.GetRandomRouter ()
					}));
		}
	}	

	void Tunnels::ManageTransitTunnels ()
	{
		uint32_t ts = i2p::util::GetSecondsSinceEpoch ();
		for (auto it = m_TransitTunnels.begin (); it != m_TransitTunnels.end ();)
		{
			if (ts > it->second->GetCreationTime () + TUNNEL_EXPIRATION_TIMEOUT)
			{
				LogPrint ("Transit tunnel ", it->second->GetTunnelID (), " expired");
				it = m_TransitTunnels.erase (it);
			}	
			else 
				it++;
		}
	}	

	void Tunnels::ManageTunnelPools ()
	{
		for (auto& it: m_Pools)
		{	
			it.second->CreateTunnels ();
			it.second->TestTunnels ();
		}
	}	
	
	void Tunnels::PostTunnelData (I2NPMessage * msg)
	{
		if (msg) m_Queue.Put (msg);		
	}	

	template<class TTunnel>
	TTunnel * Tunnels::CreateTunnel (TunnelConfig * config, OutboundTunnel * outboundTunnel)
	{
		TTunnel * newTunnel = new TTunnel (config);
		m_PendingTunnels[m_NextReplyMsgID] = newTunnel; 
		newTunnel->Build (m_NextReplyMsgID, outboundTunnel);
		m_NextReplyMsgID++; // TODO: should be atomic
		return newTunnel;
	}	

	void Tunnels::AddOutboundTunnel (OutboundTunnel * newTunnel)
	{
		m_OutboundTunnels.push_back (newTunnel);
		auto pool = newTunnel->GetTunnelPool ();
		if (pool)
			pool->TunnelCreated (newTunnel);
	}	

	void Tunnels::AddInboundTunnel (InboundTunnel * newTunnel)
	{
		m_InboundTunnels[newTunnel->GetTunnelID ()] = newTunnel;
		auto pool = newTunnel->GetTunnelPool ();
		if (!pool)
		{		
			// build symmetric outbound tunnel
			CreateTunnel<OutboundTunnel> (newTunnel->GetTunnelConfig ()->Invert (), GetNextOutboundTunnel ());		
		}
		else
			pool->TunnelCreated (newTunnel);
	}	

	
	void Tunnels::CreateZeroHopsInboundTunnel ()
	{
		CreateTunnel<InboundTunnel> (
			new TunnelConfig (std::vector<const i2p::data::RouterInfo *>
			    { 
					&i2p::context.GetRouterInfo ()
				}));
	}	
}
}
