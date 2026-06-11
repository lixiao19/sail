#ifndef WAN_OPT_HEADER_H
#define WAN_OPT_HEADER_H

#include "ns3/header.h"

namespace ns3 {

/**
 * \brief Header for WAN Optimization (Aggregation, Reliability, etc.)
 * Position: Between IPv4 and UDP.
 * Protocol Number: 253 (Use for Experimentation/Testing)
 */
class WanOptHeader : public Header {
public:
    WanOptHeader();
    virtual ~WanOptHeader();

    // Setters
    void SetAggregated(bool val);
    void SetDigest(bool val);
    void SetFake(bool val);
    void SetRepeat(bool val);
    void SetNewPsn(uint64_t psn);
    void SetNextHeader(uint8_t protocol); // Original upper-layer protocol, usually UDP 17.

    // Getters
    bool IsAggregated() const;
    bool IsDigest() const;
    bool IsFake() const;
    bool IsRepeat() const;
    uint64_t GetNewPsn() const;
    uint8_t GetNextHeader() const;

    // Standard NS-3 Methods
    static TypeId GetTypeId(void);
    virtual TypeId GetInstanceTypeId(void) const;
    virtual void Print(std::ostream &os) const;
    virtual uint32_t GetSerializedSize(void) const;
    virtual void Serialize(Buffer::Iterator start) const;
    virtual uint32_t Deserialize(Buffer::Iterator start);

private:
    // Bit 0: isAggregated
    // Bit 1: isDigest
    // Bit 2: isFake
    // Bit 3: isRepeat
    uint8_t m_flags;
    
    uint8_t m_nextHeader; // Protocol number of the next header (e.g., UDP=17)
    uint16_t m_reserved;
    uint64_t m_newPsn;    // Aggregated sequence number.
};

} // namespace ns3

#endif
