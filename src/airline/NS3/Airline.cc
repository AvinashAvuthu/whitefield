#define	_AIRLINE_CC_

#include "common.h"
#include "Airline.h"
#include "ns3/log.h"
#include "ns3/nstime.h"
#include "ns3/simulator.h"
#include "ns3/socket-factory.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/lr-wpan-module.h"

namespace ns3
{
	NS_LOG_COMPONENT_DEFINE ("AirlineApp");

	NS_OBJECT_ENSURE_REGISTERED (Airline);

	TypeId Airline::GetTypeId ()
	{
		static TypeId tid = TypeId ("ns3::Airline")
			.SetParent<Application>()
			.SetGroupName("Whitefield")
			.AddConstructor<Airline>()
			.AddAttribute ("xyz", 
					"arbitrary attribute to be defined",
					UintegerValue (100),
					MakeUintegerAccessor (&Airline::m_xyz),
					MakeUintegerChecker<uint32_t>())
			;
		return tid;
	};

	//convert 2byte short addr to Mac16Address object
	Mac16Address Airline::id2addr(const uint16_t id)
	{
		Mac16Address mac;
		uint8_t idstr[2], *ptr=(uint8_t*)&id;
		idstr[0] = ptr[0];
		idstr[1] = ptr[1];
		mac.CopyFrom(idstr);
		return mac;
	};

	uint16_t Airline::addr2id(const Mac16Address addr)
	{
		uint16_t id=0;
		uint8_t str[2], *ptr=(uint8_t *)&id;
		addr.CopyTo(str);
		ptr[0] = str[1];
		ptr[1] = str[0];
		return id;
	};

	//tx: usually called when packet is rcvd from node's stackline and to be sent on air interface
	void Airline::tx(const uint16_t dst_id, const uint8_t *pBuf, const size_t buflen)
	{
		if(pktq.size() > m_macpktqlen) {
			ERROR << (int)m_macpktqlen << " pktq size exceeded!!\n";
			return;
		}

		INFO << "sending pkt>> dst:" << dst_id 
			 << " len:" << buflen
			 << endl;
		Ptr<LrWpanNetDevice> dev = GetNode()->GetDevice(0)->GetObject<LrWpanNetDevice>();
		Ptr<Packet> p0 = Create<Packet> (pBuf, (uint32_t)buflen);
		McpsDataRequestParams params;
		params.m_srcAddrMode = SHORT_ADDR;
		params.m_dstAddrMode = SHORT_ADDR;
		params.m_dstPanId = CFG_PANID;
		params.m_dstAddr = id2addr(dst_id);
		params.m_msduHandle = 0;
		params.m_txOptions = TX_OPTION_NONE;
		if(dst_id != 0xffff) {
			params.m_txOptions = TX_OPTION_ACK;
		}
		pktq.push(params);
		Simulator::ScheduleNow (&LrWpanMac::McpsDataRequest, dev->GetMac(), params, p0);
	};

	void Airline::setDeviceAddress(void)
	{
		Mac16Address address;
		uint8_t idBuf[2];
		uint16_t id = GetNode()->GetId();
		Ptr<LrWpanNetDevice> device = GetNode()->GetDevice(0)->GetObject<LrWpanNetDevice>();

		idBuf[0] = (id >> 8) & 0xff;
		idBuf[1] = (id >> 0) & 0xff;
		address.CopyFrom (idBuf);
		device->GetMac ()->SetShortAddress (address);
	};

	void Airline::StartApplication()
	{
		//INFO << "Airline application started ID:"<< GetNode()->GetId() << endl;
		Ptr<LrWpanNetDevice> dev = GetNode()->GetDevice(0)->GetObject<LrWpanNetDevice>();
		setDeviceAddress();
		dev->GetMac()->SetMcpsDataConfirmCallback(MakeBoundCallback(&Airline::DataConfirm, this, dev));
		dev->GetMac()->SetMcpsDataIndicationCallback(MakeBoundCallback (&Airline::DataIndication, this, dev));
		SPAWN_STACKLINE(GetNode()->GetId());
	/*	if(GetNode()->GetId() == 0) {
			SendSamplePacket();
		} */
	};

	void Airline::SendSamplePacket()
	{
		uint8_t buf[50]={0};
		tx(0xffff, buf, sizeof(buf));
	};

	void Airline::StopApplication()
	{
		INFO << "Airline application stopped\n";
	};

	void Airline::SendPacketToStackline(McpsDataIndicationParams & params, Ptr<Packet> p)
	{
		DEFINE_MBUF(mbuf);
		uint16_t node_id=GetNode()->GetId();

		mbuf->src_id = addr2id(params.m_srcAddr);
		mbuf->dst_id = addr2id(params.m_dstAddr);
		mbuf->lqi = params.m_mpduLinkQuality;
		mbuf->len = p->CopyData(mbuf->buf, COMMLINE_MAX_BUF);
		if(CL_SUCCESS != cl_sendto_q(MTYPE(STACKLINE, node_id), mbuf, sizeof(msg_buf_t) + mbuf->len))
		{
			ERROR << "cl_sendto_q failed\n";
		}
	};

	void Airline::DataIndication (Airline *airline, Ptr<LrWpanNetDevice> dev, McpsDataIndicationParams params, Ptr<Packet> p)
	{
/*		INFO << "RX DATA node:" << airline->GetNode()->GetId()
			 << " LQI:" << (int)params.m_mpduLinkQuality
			 << " src:" << params.m_srcAddr
			 << " dst:" << params.m_dstAddr
			 << endl; */
		airline->SendPacketToStackline(params, p);
	};

	//Send the Ack status with retry count to stackline
	void Airline::SendAckToStackline(uint8_t m_retries) 
	{
		if(pktq.empty()) {
			ERROR << "How can the pktq be empty on dataconfirm ind?? Investigate.\n";
			return;
		}
		McpsDataRequestParams params = pktq.front();
		if(params.m_txOptions == TX_OPTION_ACK) {
			DEFINE_MBUF(mbuf);

			mbuf->src_id = GetNode()->GetId();
			mbuf->dst_id = addr2id(params.m_dstAddr);
			mbuf->lqi = m_retries;
			mbuf->flags |= MBUF_IS_ACK;
			mbuf->len = 1;
			cl_sendto_q(MTYPE(STACKLINE, mbuf->src_id), mbuf, sizeof(msg_buf_t));
		}
		pktq.pop();
	};

	//MAC layer Ack handling
	void Airline::DataConfirm (Airline *airline, Ptr<LrWpanNetDevice> dev, McpsDataConfirmParams params)
	{
	//	INFO << "RX ACK node:" << airline->GetNode()->GetId()
	//		 << " Confirm:" << params.m_status
	//		 << " Retries:" << (int)params.m_retries
	//		 << " msdu:" << (int)params.m_msduHandle
	//		 << endl;
		airline->SendAckToStackline(params.m_retries);
	};

	Airline::Airline() {
		if(!m_macpktqlen) {
			string dstr = CFG("macPktQlen");
			if(!dstr.empty()) {
				m_macpktqlen = (uint8_t)stoi(dstr);
			} else {
				m_macpktqlen = 10;
			}
		}
	};
}

