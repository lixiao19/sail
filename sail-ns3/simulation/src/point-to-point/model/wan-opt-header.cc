#include "wan-opt-header.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("WanOptHeader");
NS_OBJECT_ENSURE_REGISTERED(WanOptHeader);

WanOptHeader::WanOptHeader() 
    : m_flags(0), m_nextHeader(17), m_reserved(0), m_newPsn(0) {} // Default next is UDP

WanOptHeader::~WanOptHeader() {}

TypeId WanOptHeader::GetTypeId(void) {
    static TypeId tid = TypeId("ns3::WanOptHeader")
        .SetParent<Header>()
        .AddConstructor<WanOptHeader>();
    return tid;
}

TypeId WanOptHeader::GetInstanceTypeId(void) const {
    return GetTypeId();
}

// === Setters ===
void WanOptHeader::SetAggregated(bool val) {
    if (val) m_flags |= 0x01; else m_flags &= ~0x01;
}
void WanOptHeader::SetDigest(bool val) {
    if (val) m_flags |= 0x02; else m_flags &= ~0x02;
}
void WanOptHeader::SetFake(bool val) {
    if (val) m_flags |= 0x04; else m_flags &= ~0x04;
}
void WanOptHeader::SetRepeat(bool val) {
    if (val) m_flags |= 0x08; else m_flags &= ~0x08;
}
void WanOptHeader::SetNewPsn(uint64_t psn) { m_newPsn = psn; }
void WanOptHeader::SetNextHeader(uint8_t protocol) { m_nextHeader = protocol; }

// === Getters ===
bool WanOptHeader::IsAggregated() const { return m_flags & 0x01; }
bool WanOptHeader::IsDigest() const { return m_flags & 0x02; }
bool WanOptHeader::IsFake() const { return m_flags & 0x04; }
bool WanOptHeader::IsRepeat() const { return m_flags & 0x08; }
uint64_t WanOptHeader::GetNewPsn() const { return m_newPsn; }
uint8_t WanOptHeader::GetNextHeader() const { return m_nextHeader; }

// === Serialization ===
uint32_t WanOptHeader::GetSerializedSize(void) const {
    // Flags(1) + NextHeader(1) + Reserved(2) + NewPsn(8) = 12 Bytes
    return 12; 
}

void WanOptHeader::Serialize(Buffer::Iterator start) const {
    start.WriteU8(m_flags);
    start.WriteU8(m_nextHeader);
    start.WriteU16(m_reserved);
    start.WriteHtonU64(m_newPsn);
}

uint32_t WanOptHeader::Deserialize(Buffer::Iterator start) {
    m_flags = start.ReadU8();
    m_nextHeader = start.ReadU8();
    m_reserved = start.ReadU16(); // Read and ignore
    m_newPsn = start.ReadNtohU64();
    return GetSerializedSize();
}

void WanOptHeader::Print(std::ostream &os) const {
    os << "WanOpt(flags=" << (int)m_flags << " psn=" << m_newPsn << ")";
}

} // namespace ns3