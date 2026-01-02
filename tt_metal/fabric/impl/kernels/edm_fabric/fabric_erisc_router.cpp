// SPDX-FileCopyrightText: © 2025 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "dataflow_api.h"

#include "debug/assert.h"
//#include "debug/dprint.h"
//#include "debug/ring_buffer.h"
#include "tt_metal/hw/inc/ethernet/tunneling.h"

#include "fabric/fabric_edm_packet_header.hpp"
#include <tt-metalium/experimental/fabric/edm_fabric_counters.hpp>
#include <tt-metalium/experimental/fabric/fabric_edm_types.hpp>

#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_erisc_router_ct_args.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/edm_handshake.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_router_adapter.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_edm_packet_header_validate.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_edm_packet_transmission.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_erisc_datamover_channels.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/edm_fabric_utils.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_erisc_router_transaction_id_tracker.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_stream_regs.hpp"
#include "tt_metal/fabric/hw/inc/tt_fabric_utils.h"
#include "tt_metal/fabric/hw/inc/edm_fabric/edm_fabric_tmp_utils.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_router_flow_control.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/edm_fabric_flow_control_helpers.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_packet_recorder.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/telemetry/fabric_bandwidth_telemetry.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/telemetry/fabric_code_profiling.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_channel_traits.hpp"
#include "tt_metal/fabric/hw/inc/edm_fabric/router_data_cache.hpp"

#include "noc_overlay_parameters.h"
#include "tt_metal/hw/inc/utils/utils.h"
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_txq_setup.h"
#include "hostdevcommon/fabric_common.h"
#include "fabric_telemetry_msgs.h"
#ifdef FABRIC_2D
#include "tt_metal/fabric/hw/inc/edm_fabric/fabric_edge_node_router.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace tt::tt_fabric;

/*

The fabric Erisc Data Mover (EDM) is a component that can be used to build *very* simple linear topology fabrics.
One of these EDMs can be instantiated on each ethernet link. It is built from 3 "channels" (though the definition
of channel here is a little loose since two of the 3 will merge traffic, so this setup could be interpreted as a
two channel setup.). This EDM implements packet based packets only - concepts like sockets are not supported.

## EDM Structure

There are two sender channels and one receiver channel. "Sender" and "receiver" are relative to the Ethernet link,
not the chip. Sender sends over the link and receiver receives from the link.

Each sender channel serves a different purpose:
- Sender channel 0 : Accepts packets from a workers on the local chip
- Sender channel 1: accepts packets from an upstream EDM (i.e. an upstream
  EDM receiver channel on the same chip but different core)

The receiver channel accepts packets from the Ethernet link and can do one (or both) of:
- Write the packet to local chip if it is the intended destination (unicast or mcast)
- Forward the packet to the next chip in the line if:
  - Unicast and not the target chip
  - Multicast and this chip is in the multicast target range

Sender channels will merge traffic into the remote EDM's receiver channel.

Below is a diagram that shows how EDMs can be connected over an ethernet link. In this case, the two
EDM kernels are run on separate, but connected ethernet link cores.

 ┌───────────────────────┐           ┌───────────────────────┐
 │    Sender Channel 0   │           │    Receiver Channel   │
 │   ┌────────────────┐  │           │   ┌────────────────┐  │
 │   │                ┼──┼───┬───────┼───►                │  │
 │   │                │  │   │       │   │                │  │
 │   └────────────────┘  │   │       │   └────────────────┘  │
 │    Sender Channel 1   │   │       │    Sender Channel 1   │
 │   ┌────────────────┐  │   │       │   ┌────────────────┐  │
 │   │                ┼──┼───┘       │   │                │  │
 │   │                │  │         ┌─┼───┼                │  │
 │   └────────────────┘  │         │ │   └────────────────┘  │
 │    Receiver Channel   │         │ │    Sender Channel 0   │
 │   ┌────────────────┐  │         │ │   ┌────────────────┐  │
 │   │                │  │         │ │   │                │  │
 │   │                ◄──┼─────────┴─┼───┼                │  │
 │   └────────────────┘  │           │   └────────────────┘  │
 │                       │           │                       │
 │                       │           │                       │
 └───────────────────────┘           └───────────────────────┘


## Building a "Fabric"

At present, only linear topologies are supported, and one per ethernet link along that given line.
Below shows the intended connectivity of EDMs across chips in a hypothetical 3-chip fabric. For longer
lines, the pattern would be extended.

           CHIP 0                              CHIP 1                             CHIP 2
     ┌─────────────────┐                ┌─────────────────┐                ┌─────────────────┐
     │                 │                │                 │                │                 │
┌────┴─────┐ ▲   ┌─────┴────┐      ┌────┴─────┐ ▲   ┌─────┴────┐      ┌────┴─────┐ ▲   ┌─────┴────┐
│   EDM    │ │   │   EDM    │      │   EDM    │ │   │   EDM    │      │   EDM    │ │   │   EDM    │
│ ┌──────┐ │ │   │ ┌──────┐ │      │ ┌──────┐ │ │   │ ┌──────┐ │      │ ┌──────┐ │ │   │ ┌──────┐ │
│ │ Rx   ┼─┼─┴───┼─► S1   ┼─┼─┬────┼─► Rx   ┼─┼─┴───┼─► S1   ┼─┼┬─────┼─► Rx   ┼─┼─┘   | | S1   │ │
│ └──────┘ │     │ └──────┘ │ │    │ └──────┘ │     │ └──────┘ ││     │ └──────┘ │     │ └──────┘ │
│ ┌──────┐ │     │ ┌──────┐ │ │    │ ┌──────┐ │     │ ┌──────┐ ││     │ ┌──────┐ │     │ ┌──────┐ │
│ │ S0   ◄─┼──┬──┼─► S0   ┼─┼─┘   ┌┼─┼ S0   ◄─┼──┬──┼─► S0   ┼─┼┘    ┌┼─┼ S0   ◄─┼──┬──┼─► S0   │ │
│ └──────┘ │  │  │ └──────┘ │     ││ └──────┘ │  │  │ └──────┘ │     ││ └──────┘ │  │  │ └──────┘ │
│ ┌──────┐ │  │  │ ┌──────┐ │     ││ ┌──────┐ │  │  │ ┌──────┐ │     ││ ┌──────┐ │  │  │ ┌──────┐ │
│ │ S1   | |  │ ┌┼─┼ Rx   ◄─┼─────┴┼─┼ S1   ◄─┼─┐│ ┌┼─┼ Rx   ◄─┼─────┴┼─┼ S1   ◄─┼─┐│ ┌┼─┼ Rx   │ │
│ └──────┘ │  | |│ └──────┘ │      │ └──────┘ │ └┼─┤│ └──────┘ │      │ └──────┘ │ └┼─┤│ └──────┘ │
└────┬─────┘  │ │└─────┬────┘      └────┬─────┘  │ │└─────┬────┘      └────┬─────┘  │ │└─────┬────┘
     │          ▼      │                │          ▼      │                │          ▼      │
     └─────────────────┘                └─────────────────┘                └─────────────────┘


## Connecting Workers to Channels

As mentioned, only one worker can push to a given EDM sender channel at a time. In order to send to an EDM
sender channel, the worker must establish a connection. The connection protocol is as follows and is started
by the worker (the EDM is a subordinate in this protocol).

*NOTE*: If multiple workers try to connect to the same EDM sender channel at the same time, the behavior is undefined.
*NOTE*: Additionally, if a worker pushes packets to a channel it isn't connected to, behaviour is undefined.
*NOTE*: Undefined == likely hang

The `EdmToEdmSender` from `edm_fabric_worker_adapters.hpp`
provides an implementation of the connection protocol. `EdmToEdmSender` also acts as a wrapper around that
protocol so workers can simply call `open()` to execute the connection protocol without having to manually reimplement
for each kernel.

### Protocol
Worker:
- Read from EDM sender channel buffer_index address
  - Required so that the worker knows where to write its first packet (since the channel may already contain packets
from a previous connection)
- Write worker core X/Y (NOC 0 based)
- Write worker flow control semaphore L1 address

EDM Sender Channel:
- Check local connection valid semaphore for new established connection
  - When the connection semaphore indicates an active connection, the channel assumes all other relevant fields were
    correctly populated by the worker:
    - Worker core_x (on NOC 0)
    - Worker core_y (on NOC 0)
    - Worker flow control semaphore L1 address


## Tearing Down Connections

Every worker is required to explicitly teardown its connection with the EDM before terminating. To do this, the worker
must simply write a `0` to the EDM sender channel's connection semaphore address. As long as the worker has sent all
of its packets to the EDM before this, then the EDM will guarantee to forward the messages correctly.

At this point, it is safe for another kernel to establish a connection.

## Packet Structure

Workers are responsible for populating packet headers before sending to the EDM. The packet header structure is defined
in `fabric_edm_packet_header.hpp`.

## Channel structure

Each EDM channel is built from one or more buffers. Each buffer is the same size and can hold at most one packet.
Neighbouring packets occupy nehighouring buffers - with the exception of the last buffer index. The next packet after a
write into the last buffer index will wrap around to the first buffer index. Even if packets do not occupy the full
buffer, subsequent packets will always be written into the next logical buffer. A gap will exist in memory but the EDM
will not send that padded data (unless it is more performant - which is possible in some special cases)

 Example channel with 8 buffers
┌───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┐
│       │       │       │       │       │       │       │       │
│       │       │       │       │       │       │       │       │
└───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘
 buf 0   buf 1   buf 2   buf 3   buf 4   buf 5   buf 6   buf 7


Here we have an example of a channel with 4 buffers, filled with some number of packets. Each packet is a different
size. Packets 0, 2, and 3 are smaller than the full buffer size, while packet 1 is the full buffer size.

┌───────────────┬───────────────┬───────────────┬───────────────┐
│H|Payload| / / │H|Payload      │H|Pyld| / / / /│H|Payload  |/ /│
│ |       |/ / /│ |             │ |    |/ / / / │ |         | / │
└───────────────┴───────────────┴───────────────┴───────────────┘
  buf 0           buf 1           buf 2           buf 3




## Sending Packets
Sending a packet is done as follows:

1) Worker waits for flow control semaphore increment from EDM sender channel
  - Indicates there is space at the next buffer index for a packet
2) Worker performs a noc write of its packet to the EDM sender channel at the buffer index

*NOTE*: !!!ALL PACKETS MUST CONTAIN DESTINATION NOC X/Y AS NOC 0 COORDINATES, REGARDLESS OF THE `noc_index` OF THE
SENDER!!!


## EDM <-> EDM Channel Flow Control
The flow control protocol between EDM channels is built on a rd/wr ptr based protocol where pointers are
to buffer slots within the channel (as opposed so something else like byte or word offset). Ptrs are
free to advance independently from each other as long as there is no overflow or underflow.

The flow control is implemented through the use of several stream registers: one per conceptual pointer being tracked.
In total there are 5 such counters:
1) to receiver channel packets sent
  - Incremented by sender (via eth_reg_write) by the number of buffer slots written. In practice, this means it is
    incremented once per packet
2) to sender 0 packets acked
  - Incremented by receiver for every new packet from channel 0 that it sees
3) to sender 1 packets acked
  - Incremented by receiver for every new packet from channel 1 that it sees
4) to sender 0 packets completed
  - Incremented by receiver for every packet from channel 0 that it completes processing for
5) to sender 1 packets completed
  - Incremented by receiver for every packet from channel 1 that it completes processing for

See calls to `increment_local_update_ptr_val`, `remote_update_ptr_val`, `init_ptr_val` for more on implementation.

### Sender Channel Flow Control
Both sender channels share the same flow control view into the receiver channel. This is because both channels
write to the same receiver channel.
* wrptr:
  * points to next buffer slot to write to into the remote (over Ethernet) receiver channel.
  * leads other pointers
  * writer updates for every new packet
  * `has_data_to_send(): local_wrptr != remote_sender_wrptr`
* ackptr
  * trails `wrptr`
  * advances as the channel receives acknowledgements from the receiver
    * as this advances, the sender channel can notify the upstream worker of additional space in sender channel buffer
* completion_ptr:
  * trails `local_wrptr`
  * "rdptr" from remote sender's perspective
  * advances as packets completed by receiver
    * as this advances, the sender channel can write additional packets to the receiver at this slot

### Receiver Channel Flow Control
* ackptr/rdptr:
  * leads all pointers
  * indicates the next buffer slot we expect data to arrive (from remote sender) at
    * advances as packets are received (and acked)
  * make sure not to overlap completion pointer
* wr_sent_ptr:
  * trails `ackptr`
  * indicates the buffer slot currently being processed, written out
    * advances after all forwding writes (to noc or downstream EDM) are initiated
* wr_flush_ptr:
  * trails `wr_sent_ptr`
  * advances as writes are flushed
* completion_ptr:
  * trails `wr_flush_ptr`
  * indicates the next receiver buffer slot in the receiver channel to send completion acks for
*/

////////////////////////////////////////////////
// Data structures, types, enums, and constants
////////////////////////////////////////////////

template <typename HEADER_TYPE, uint8_t NUM_BUFFERS>
using SenderEthChannel = StaticSizedSenderEthChannel<HEADER_TYPE, NUM_BUFFERS>;

static constexpr bool const PERF_TELEMETRY_DISABLED = (perf_telemetry_mode == PerfTelemetryRecorderType::NONE);
static constexpr bool const PERF_TELEMETRY_LOW_RESOLUTION_BANDWIDTH =
    (perf_telemetry_mode == PerfTelemetryRecorderType::LOW_RESOLUTION_BANDWIDTH);

using PerfTelemetryRecorder = std::conditional_t<
    PERF_TELEMETRY_LOW_RESOLUTION_BANDWIDTH,
    LowResolutionBandwidthTelemetry,
    std::conditional_t<PERF_TELEMETRY_DISABLED, bool, std::nullptr_t>>;

// Currently, we enable elastic channels in an all-or-nothing manner for router -> router
// connections.

constexpr bool ANY_SENDER_CHANNELS_ARE_ELASTIC() {
    // Manually unroll loop for compile-time evaluation - typically 2-8 channels
    if (IS_ELASTIC_SENDER_CHANNEL[0U]) {
        return true;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 1U) {
        if (IS_ELASTIC_SENDER_CHANNEL[1U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 2U) {
        if (IS_ELASTIC_SENDER_CHANNEL[2U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 3U) {
        if (IS_ELASTIC_SENDER_CHANNEL[3U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 4U) {
        if (IS_ELASTIC_SENDER_CHANNEL[4U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 5U) {
        if (IS_ELASTIC_SENDER_CHANNEL[5U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 6U) {
        if (IS_ELASTIC_SENDER_CHANNEL[6U]) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 7U) {
        if (IS_ELASTIC_SENDER_CHANNEL[7U]) {
            return true;
        }
    }
    
    return false;
}

constexpr bool const PERSISTENT_SENDER_CHANNELS_ARE_ELASTIC = ANY_SENDER_CHANNELS_ARE_ELASTIC();

// Stubbed out the elastic channel writer adapter until elastic channels implemented
// Issue: https://github.com/tenstorrent/tt-metal/issues/26311
template <uint8_t SLOTS_PER_CHUNK, uint16_t CHUNK_SIZE_BYTES>
struct RouterElasticChannelWriterAdapter {};

template <uint8_t SENDER_NUM_BUFFERS>
using RouterToRouterSender = std::conditional_t<
    PERSISTENT_SENDER_CHANNELS_ARE_ELASTIC,
    tt::tt_fabric::RouterElasticChannelWriterAdapter<CHUNK_N_PKTS, channel_buffer_size>,
    tt::tt_fabric::EdmToEdmSender<SENDER_NUM_BUFFERS>>;

constexpr bool is_spine_direction(eth_chan_directions const direction) {
    //    return direction == eth_chan_directions::NORTH || direction == eth_chan_directions::SOUTH;
    // Branchless check: NORTH=2 (0b10) and SOUTH=3 (0b11) both have bit 1 set
    // This avoids branch instructions on RV32I, using single AND + compare-with-zero
    return (static_cast<uint32_t>(direction) & 0x2U) != 0U;
}

alignas(sizeof(uint32_t)) static constexpr std::array<uint32_t, MAX_NUM_SENDER_CHANNELS> sender_channel_free_slots_stream_ids = {
    sender_channel_0_free_slots_stream_id,
    sender_channel_1_free_slots_stream_id,
    sender_channel_2_free_slots_stream_id,
    sender_channel_3_free_slots_stream_id,
    sender_channel_4_free_slots_stream_id,
    sender_channel_5_free_slots_stream_id,
    sender_channel_6_free_slots_stream_id,
    sender_channel_7_free_slots_stream_id
};
static_assert(sender_channel_free_slots_stream_ids[0U] == 21U);
static_assert(sender_channel_free_slots_stream_ids[1U] == 22U);
static_assert(sender_channel_free_slots_stream_ids[2U] == 23U);
static_assert(sender_channel_free_slots_stream_ids[3U] == 24U);
static_assert(sender_channel_free_slots_stream_ids[4U] == 25U);
static_assert(sender_channel_free_slots_stream_ids[5U] == 26U);
static_assert(sender_channel_free_slots_stream_ids[6U] == 27U);
static_assert(sender_channel_free_slots_stream_ids[7U] == 28U);

// For 2D fabric: maps compact index to downstream direction for each my_direction
// For 1D fabric: only 1 downstream direction per router (EAST forwards to WEST in 1D linear topology)
#if defined(FABRIC_2D)
alignas(sizeof(uint32_t)) constexpr static uint32_t edm_index_to_edm_direction[eth_chan_directions::COUNT][NUM_DOWNSTREAM_SENDERS_VC0] = {
    {eth_chan_directions::WEST, eth_chan_directions::NORTH, eth_chan_directions::SOUTH},  // EAST router
    {eth_chan_directions::EAST, eth_chan_directions::NORTH, eth_chan_directions::SOUTH},  // WEST router
    {eth_chan_directions::EAST, eth_chan_directions::WEST, eth_chan_directions::SOUTH},   // NORTH router
    {eth_chan_directions::EAST, eth_chan_directions::WEST, eth_chan_directions::NORTH},   // SOUTH router
};

// sender_channel_free_slots_stream_ids[] mapping:
//   [0] → Local worker (always uses sender channel 0 on the outgoing router).
//   [1–3] → Sender channels 1–3 on the outgoing router, corresponding to
//           inbound traffic from neighboring routers.
//
// The mapping is relative to the outgoing router's direction:
//
//   • East-outbound router:
//         sender channel 1 (idx 0) ← West inbound
//         sender channel 2 (idx 1) ← North inbound
//         sender channel 3 (idx 2) ← South inbound
//
//   • West-outbound router:
//         sender channel 1 (idx 0) ← East inbound
//         sender channel 2 (idx 1) ← North inbound
//         sender channel 3 (idx 2) ← South inbound
//
//   • North-outbound router:
//         sender channel 1 (idx 0) ← East inbound
//         sender channel 2 (idx 1) ← West inbound
//         sender channel 3 (idx 2) ← South inbound
//
//   • South-outbound router:
//         sender channel 1 (idx 0) ← East inbound
//         sender channel 2 (idx 1) ← West inbound
//         sender channel 3 (idx 2) ← North inbound
constexpr uint32_t get_vc0_downstream_sender_channel_free_slots_stream_id(uint32_t const compact_index) {
    auto ds_edm_direction = edm_index_to_edm_direction[my_direction][compact_index];
    if (my_direction > ds_edm_direction) {
        // downstream sender channel = my_direction
        // stream id = sender_channel_free_slots_stream_ids[downstream sender channel]
        return sender_channel_free_slots_stream_ids[my_direction];
    } else {
        // downstream sender channel = my_direction + 1
        // stream id = sender_channel_free_slots_stream_ids[downstream sender channel]
        return sender_channel_free_slots_stream_ids[(1U + my_direction)];
    }
}
#endif

FORCE_INLINE constexpr eth_chan_directions map_compact_index_to_direction(size_t const compact_index) {
#if defined(FABRIC_2D)
    return static_cast<eth_chan_directions>(edm_index_to_edm_direction[my_direction][compact_index]);
#else
    return static_cast<eth_chan_directions>(compact_index);
#endif
}

// Determine which sender channels are "turn" channels (i.e., north/south for east/west routers)
// Channel 0 is always for local workers, so it's never a turn channel
// For 2D fabric, channels 1-3 correspond to compact indices 0-2, which map to actual directions
constexpr auto get_sender_channel_turn_statuses() -> std::array<bool, MAX_NUM_SENDER_CHANNELS_VC0> {
    std::array<bool, MAX_NUM_SENDER_CHANNELS_VC0> turn_statuses = {};  // Zero-initialize all elements
    
    // Channel 0 is always for local workers, never a turn channel
    // Only non-spine routers (EAST/WEST) have turn channels
    if constexpr (!is_spine_direction(static_cast<eth_chan_directions>(my_direction))) {
        // Manually unroll loop for RV32I optimization (channels 1-3)
        // Sender channel i corresponds to compact index (i-1)
        constexpr const size_t compact_index_0 = 0U;
        const auto down_direction_0 = map_compact_index_to_direction(compact_index_0);
        const auto turn_status_0 = is_spine_direction(down_direction_0);
        turn_statuses[1U] = turn_status_0;
        
        if constexpr (MAX_NUM_SENDER_CHANNELS_VC0 > 2) {
            constexpr const size_t compact_index_1 = 1U;
            const auto down_direction_1 = map_compact_index_to_direction(compact_index_1);
            const auto turn_status_1 = is_spine_direction(down_direction_1);
            turn_statuses[2U] = turn_status_1;
        }
        
        if constexpr (MAX_NUM_SENDER_CHANNELS_VC0 > 3) {
            constexpr const size_t compact_index_2 = 2U;
            const auto down_direction_2 = map_compact_index_to_direction(compact_index_2);
            const auto turn_status_2 = is_spine_direction(down_direction_2);
            turn_statuses[3U] = turn_status_2;
        }
    }

    return turn_statuses;
}

// Map downstream direction to compact array index [0-2], excluding my_direction
// This function assumes 2D fabric where routers don't forward to themselves
// Examples:
// - EAST router (my_direction=0): WEST(1)→0, NORTH(2)→1, SOUTH(3)→2
// - WEST router (my_direction=1): EAST(0)→0, NORTH(2)→1, SOUTH(3)→2
// - NORTH router (my_direction=2): EAST(0)→0, WEST(1)→1, SOUTH(3)→2
// - SOUTH router (my_direction=3): EAST(0)→0, WEST(1)→1, NORTH(2)→2
alignas(sizeof(uint32_t)) constexpr static const size_t direction_to_compact_index_map[eth_chan_directions::COUNT][eth_chan_directions::COUNT] = {
    {0U, 0U, 1U, 2U},  // EAST router -> WEST, NORTH, SOUTH
    {0U, 0U, 1U, 2U},  // WEST router -> EAST, NORTH, SOUTH
    {0U, 1U, 0U, 2U},  // NORTH router -> EAST, WEST, SOUTH
    {0U, 1U, 2U, 0U},  // SOUTH router -> EAST, WEST, NORTH
};

template <eth_chan_directions downstream_direction>
FORCE_INLINE constexpr size_t map_downstream_direction_to_compact_index() {
    return direction_to_compact_index_map[my_direction][downstream_direction];
}

FORCE_INLINE constexpr size_t map_downstream_direction_to_compact_index(eth_chan_directions const downstream_direction) {
    return direction_to_compact_index_map[my_direction][downstream_direction];
}

static constexpr const std::array<bool, MAX_NUM_SENDER_CHANNELS_VC0> sender_channels_turn_status =
    get_sender_channel_turn_statuses();

static constexpr const std::array<uint32_t, NUM_ROUTER_CARDINAL_DIRECTIONS> vc_0_free_slots_stream_ids = {
    vc_0_free_slots_from_downstream_edge_1_stream_id,
    vc_0_free_slots_from_downstream_edge_2_stream_id,
    vc_0_free_slots_from_downstream_edge_3_stream_id,
    0};

enum PacketLocalForwardType : uint8_t {
    PACKET_FORWARD_INVALID = 0x0U,
    PACKET_FORWARD_LOCAL_ONLY = 0x1U,
    PACKET_FORWARD_REMOTE_ONLY = 0x2U,
    PACKET_FORWARD_LOCAL_AND_REMOTE = 0x3U
};

// tracks if the main loop made any progress. If many loop iterations were completed without
// did_something=true (i.e. no progress was made), then we allow for context switch in case
// the link is down
static bool did_something;

/////////////////////////////////////////////
//   SENDER SIDE HELPERS
/////////////////////////////////////////////

// Add helper function
template <uint8_t SENDER_CHANNEL_INDEX>
FORCE_INLINE void update_packet_header_before_eth_send(volatile tt_l1_ptr PACKET_HEADER_TYPE* packet_header) {
#if defined(FABRIC_2D)
    constexpr bool const IS_FORWARDED_TRAFFIC_FROM_ROUTER = SENDER_CHANNEL_INDEX != 0;
    constexpr bool const IS_TURN = sender_channels_turn_status[SENDER_CHANNEL_INDEX];
    static_assert(
        my_direction == eth_chan_directions::EAST || my_direction == eth_chan_directions::WEST ||
        my_direction == eth_chan_directions::NORTH || my_direction == eth_chan_directions::SOUTH);
    static_assert(
        is_spine_direction(eth_chan_directions::NORTH) || is_spine_direction(eth_chan_directions::SOUTH),
        "Only spine direction of NORTH and SOUTH is supported with this code. If additional spine directions are being "
        "added, please update the code below to support them.");
    if constexpr (IS_FORWARDED_TRAFFIC_FROM_ROUTER) {
        ROUTING_FIELDS_TYPE cached_routing_fields;
        cached_routing_fields.value = packet_header->routing_fields.value;

        if constexpr (IS_TURN) {
            if constexpr (my_direction == eth_chan_directions::EAST) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            } else {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
        } else {
            cached_routing_fields.value = cached_routing_fields.value + 1;
        }
        packet_header->routing_fields.value = cached_routing_fields.value;
    }
#endif
}

template <
    uint8_t sender_channel_index,
    uint8_t to_receiver_pkts_sent_id,
    bool SKIP_CONNECTION_LIVENESS_CHECK,
    typename SenderChannelT,
    typename WorkerInterfaceT,
    typename ReceiverChannelT>
FORCE_INLINE void send_next_data(
    SenderChannelT & sender_buffer_channel,
    WorkerInterfaceT& sender_worker_interface,
    uint32_t& outbound_to_receiver_channel_pointers_num_free_slots,
    ReceiverChannelT& receiver_buffer_channel,
    PerfTelemetryRecorder& perf_telemetry_recorder) {

    uint32_t const src_addr = sender_buffer_channel.get_cached_next_buffer_slot_addr();

    union {
        volatile PACKET_HEADER_TYPE* pkt_header;
        uintptr_t addr;
    } src_addr_cast;
    src_addr_cast.addr = src_addr;

    volatile PACKET_HEADER_TYPE* pkt_header = src_addr_cast.pkt_header;
    size_t const payload_size_bytes = pkt_header->get_payload_size_including_header();
    
    auto const dest_addr = receiver_buffer_channel.get_cached_next_buffer_slot_addr();

    if constexpr (!skip_src_ch_id_update) {
        pkt_header->src_ch_id = sender_channel_index;
    }

    if constexpr (ETH_TXQ_SPIN_WAIT_SEND_NEXT_DATA) {
        while (internal_::eth_txq_is_busy(sender_txq_id)) {
        };
    }
    internal_::eth_send_packet_bytes_unsafe(sender_txq_id, src_addr, dest_addr, payload_size_bytes);

    // Note: We can only advance to the next buffer index if we have fully completed the send (both the payload and sync
    // messages)
    sender_worker_interface.template update_write_counter_for_send<SKIP_CONNECTION_LIVENESS_CHECK>();

    receiver_buffer_channel.advance_remote_receiver_buffer_index();
    sender_buffer_channel.advance_to_next_cached_buffer_slot_addr();
    
    outbound_to_receiver_channel_pointers_num_free_slots--;

    record_packet_send(perf_telemetry_recorder, sender_channel_index, payload_size_bytes);

    while (internal_::eth_txq_is_busy(sender_txq_id)) {
    };

    remote_update_ptr_val<to_receiver_pkts_sent_id, sender_txq_id>(1);
}

/////////////////////////////////////////////
//   RECEIVER SIDE HELPERS
/////////////////////////////////////////////


template <typename DownstreamSenderT>
FORCE_INLINE bool can_forward_packet_completely(
    ROUTING_FIELDS_TYPE const cached_routing_fields, DownstreamSenderT const& downstream_edm_interface) {
    // We always check if it is the terminal mcast packet value. We can do this because all unicast packets have the
    // mcast terminal value masked in to the routing field. This simplifies the check here to a single compare.
    bool deliver_locally_only;
    if constexpr (std::is_same_v<ROUTING_FIELDS_TYPE, tt::tt_fabric::RoutingFields>) {
        deliver_locally_only = cached_routing_fields.value == tt::tt_fabric::RoutingFields::LAST_MCAST_VAL;
    } else if constexpr (std::is_same_v<ROUTING_FIELDS_TYPE, tt::tt_fabric::LowLatencyRoutingFields>) {
        deliver_locally_only = (cached_routing_fields.value & tt::tt_fabric::LowLatencyRoutingFields::FIELD_MASK) ==
                               tt::tt_fabric::LowLatencyRoutingFields::WRITE_ONLY;
    }
    return deliver_locally_only || downstream_edm_interface.template edm_has_space_for_packet<ENABLE_RISC_CPU_DATA_CACHE>();
}

template <eth_chan_directions downstream_direction>
FORCE_INLINE constexpr size_t get_downstream_edm_interface_index() {
    // Map downstream direction to compact array index (excluding router's own direction)
    size_t const downstream_edm_interface_index = map_downstream_direction_to_compact_index<downstream_direction>();

    return downstream_edm_interface_index;
}

FORCE_INLINE constexpr size_t get_downstream_edm_interface_index(eth_chan_directions const downstream_direction) {
    // Map downstream direction to compact array index (excluding router's own direction)
    return map_downstream_direction_to_compact_index(downstream_direction);
}

template <typename DownstreamSenderVC0T, eth_chan_directions DIRECTION>
FORCE_INLINE bool check_downstream_has_space(
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0> const& downstream_edm_interfaces_vc0) {
    if constexpr (DIRECTION == my_direction) {
        return true;
    } else {
        constexpr auto edm_index = get_downstream_edm_interface_index(DIRECTION);
        return downstream_edm_interfaces_vc0[edm_index].template edm_has_space_for_packet<ENABLE_RISC_CPU_DATA_CACHE>();
    }
}

template <typename DownstreamSenderVC0T, typename LocalRelayInterfaceT, eth_chan_directions DIRECTION>
FORCE_INLINE bool check_downstream_has_space(
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0> const& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT const& local_relay_interface) {
    if constexpr (DIRECTION == my_direction) {
        if constexpr (udm_mode) {
            return local_relay_interface.template edm_has_space_for_packet<ENABLE_RISC_CPU_DATA_CACHE>();
        } else {
            return true;
        }
    } else {
        constexpr auto edm_index = get_downstream_edm_interface_index<DIRECTION>();
        return downstream_edm_interfaces_vc0[edm_index].template edm_has_space_for_packet<ENABLE_RISC_CPU_DATA_CACHE>();
    }
}

template <typename DownstreamSenderVC0T, typename LocalRelayInterfaceT, eth_chan_directions... DIRECTIONS>
FORCE_INLINE bool downstreams_have_space(
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0> const& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT const& local_relay_interface) {
    return (
        ... && check_downstream_has_space<DownstreamSenderVC0T, LocalRelayInterfaceT, DIRECTIONS>(
                   downstream_edm_interfaces_vc0, local_relay_interface));
}

#ifdef FABRIC_2D
template <typename DownstreamSenderVC0T, typename LocalRelayInterfaceT>
FORCE_INLINE __attribute__((optimize("jump-tables"))) bool can_forward_packet_completely(
    uint32_t const hop_cmd,
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0> const& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT const& local_relay_interface) {
    bool ret_val = false;

    using eth_chan_directions::EAST;
    using eth_chan_directions::NORTH;
    using eth_chan_directions::SOUTH;
    using eth_chan_directions::WEST;

    switch (hop_cmd) {
        case LowLatencyMeshRoutingFields::NOOP: break;
        case LowLatencyMeshRoutingFields::FORWARD_EAST:
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::FORWARD_WEST:
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, WEST>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_EW:
            // Line Mcast East<->West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, WEST>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::FORWARD_NORTH:
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, NORTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::FORWARD_SOUTH:
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NS:
            // Line Mcast North<->South
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, NORTH, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSEW:
            // 2D Mcast Trunk: North<->South
            // 2D Mcast Branch: East and West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, WEST, NORTH, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSE:
            // 2D Mcast Trunk: North<->South
            // 2D Mcast Branch: East
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, NORTH, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSW:
            // 2D Mcast Trunk: North<->South
            // 2D Mcast Branch: West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, WEST, NORTH, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SEW:
            // 2D Mcast Trunk: Last hop North
            // 2D Mcast Branch: East and West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, WEST, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NEW:
            // 2D Mcast Trunk: Last hop South
            // 2D Mcast Branch: East and West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, WEST, NORTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SE:
            // 2D Mcast Trunk: Last hop North
            // 2D Mcast Branch: East
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SW:
            // 2D Mcast Trunk: Last hop North
            // 2D Mcast Branch: West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, WEST, SOUTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NE:
            // 2D Mcast Trunk: Last hop South
            // 2D Mcast Branch: East
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, EAST, NORTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NW:
            // 2D Mcast Trunk: Last hop South
            // 2D Mcast Branch: West
            ret_val = downstreams_have_space<DownstreamSenderVC0T, LocalRelayInterfaceT, WEST, NORTH>(
                downstream_edm_interfaces_vc0, local_relay_interface);
            break;
        default: __builtin_unreachable();
    }
    return ret_val;
}

#else

// !!!WARNING!!! - MAKE SURE CONSUMER HAS SPACE BEFORE CALLING
template <uint8_t rx_channel_id, typename DownstreamSenderT>
FORCE_INLINE void receiver_forward_packet(
    // TODO: have a separate cached copy of the packet header to save some additional L1 loads
    tt_l1_ptr PACKET_HEADER_TYPE* packet_start,
    ROUTING_FIELDS_TYPE const cached_routing_fields,
    DownstreamSenderT& downstream_edm_interface,
    uint8_t const transaction_id) {
    constexpr bool ENABLE_STATEFUL_NOC_APIS =
#if !defined(DEBUG_PRINT_ENABLED) and !defined(WATCHER_ENABLED)
        !FORCE_ALL_PATHS_TO_USE_SAME_NOC && true;
#else
        false;
#endif
    router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();  // Make sure we have the latest packet header in L1
    if constexpr (std::is_same_v<ROUTING_FIELDS_TYPE, tt::tt_fabric::RoutingFields>) {
        // If the packet is a terminal packet, then we can just deliver it locally
        bool const start_distance_is_terminal_value =
            (cached_routing_fields.value & tt::tt_fabric::RoutingFields::HOP_DISTANCE_MASK) ==
            tt::tt_fabric::RoutingFields::LAST_HOP_DISTANCE_VAL;
        uint16_t const payload_size_bytes = packet_start->payload_size_bytes;
        bool const not_last_destination_device = cached_routing_fields.value != tt::tt_fabric::RoutingFields::LAST_MCAST_VAL;
        // disable when dprint enabled due to noc cmd buf usage of DPRINT
        if (not_last_destination_device) {
            forward_payload_to_downstream_edm<enable_deadlock_avoidance, ENABLE_STATEFUL_NOC_APIS>(
                packet_start, payload_size_bytes, cached_routing_fields, downstream_edm_interface, transaction_id);
        }
        if (start_distance_is_terminal_value) {
            execute_chip_unicast_to_local_chip(packet_start, payload_size_bytes, transaction_id, rx_channel_id);
        }
    } else if constexpr (std::is_same_v<ROUTING_FIELDS_TYPE, tt::tt_fabric::LowLatencyRoutingFields>) {
        const uint64_t routing_value = cached_routing_fields.value;
        const uint32_t routing_value_low = static_cast<uint32_t>(routing_value);
        const auto routing = routing_value_low & tt::tt_fabric::LowLatencyRoutingFields::FIELD_MASK;
        uint16_t const payload_size_bytes = packet_start->payload_size_bytes;
        switch (routing) {
            case tt::tt_fabric::LowLatencyRoutingFields::WRITE_ONLY:
                execute_chip_unicast_to_local_chip(packet_start, payload_size_bytes, transaction_id, rx_channel_id);
                break;
            case tt::tt_fabric::LowLatencyRoutingFields::FORWARD_ONLY:
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, ENABLE_STATEFUL_NOC_APIS>(
                    packet_start, payload_size_bytes, cached_routing_fields, downstream_edm_interface, transaction_id);
                break;
            case tt::tt_fabric::LowLatencyRoutingFields::WRITE_AND_FORWARD:
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, ENABLE_STATEFUL_NOC_APIS>(
                    packet_start, payload_size_bytes, cached_routing_fields, downstream_edm_interface, transaction_id);
                execute_chip_unicast_to_local_chip(packet_start, payload_size_bytes, transaction_id, rx_channel_id);
                break;
            default: {
                ASSERT(false);
            }
        }
    }
}

#endif

#if defined(FABRIC_2D)

// Helper to forward packet to local destination
// (relay in UDM mode, or local chip directly in non-UDM mode)
template <uint8_t rx_channel_id, typename LocalRelayInterfaceT>
FORCE_INLINE void forward_to_local_destination(
    LocalRelayInterfaceT& local_relay_interface,
    tt_l1_ptr PACKET_HEADER_TYPE* packet_start,
    uint16_t const payload_size_bytes,
    uint8_t const transaction_id) {
    if constexpr (udm_mode) {
        execute_chip_unicast_to_relay(
            local_relay_interface, packet_start, payload_size_bytes, transaction_id, rx_channel_id);
    } else {
        execute_chip_unicast_to_local_chip(packet_start, payload_size_bytes, transaction_id, rx_channel_id);
    }
}

// !!!WARNING!!! - MAKE SURE CONSUMER HAS SPACE BEFORE CALLING
template <uint8_t rx_channel_id, typename DownstreamSenderVC0T, typename LocalRelayInterfaceT>
FORCE_INLINE __attribute__((optimize("jump-tables"))) void receiver_forward_packet(
    tt_l1_ptr PACKET_HEADER_TYPE* packet_start,
    ROUTING_FIELDS_TYPE & cached_routing_fields,
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0>& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT& local_relay_interface,
    uint8_t const transaction_id,
    uint32_t const hop_cmd) {
    uint16_t const payload_size_bytes = packet_start->payload_size_bytes;

    using eth_chan_directions::EAST;
    using eth_chan_directions::NORTH;
    using eth_chan_directions::SOUTH;
    using eth_chan_directions::WEST;

    switch (hop_cmd) {
        case LowLatencyMeshRoutingFields::NOOP: break;
        case LowLatencyMeshRoutingFields::FORWARD_EAST:
            if constexpr (my_direction == EAST) {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            } else {
                constexpr auto edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::FORWARD_WEST:
            if constexpr (my_direction == WEST) {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            } else {
                constexpr auto edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_EW:
            if constexpr (my_direction == WEST) {
                constexpr auto edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                constexpr auto edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            forward_to_local_destination<rx_channel_id>(
                local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            break;
        case LowLatencyMeshRoutingFields::FORWARD_NORTH:
            if constexpr (my_direction == NORTH) {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            } else {
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::FORWARD_SOUTH:
            if constexpr (my_direction == SOUTH) {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            } else {
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NS:
            if constexpr (my_direction == SOUTH) {
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            forward_to_local_destination<rx_channel_id>(
                local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSEW:
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.value++;
            }
            if constexpr (my_direction == SOUTH) {
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                constexpr auto edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            forward_to_local_destination<rx_channel_id>(
                local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSE:
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.value++;
            }
            if constexpr (my_direction == SOUTH) {
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            forward_to_local_destination<rx_channel_id>(
                local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NSW:
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.value++;
            }
            if constexpr (my_direction == SOUTH) {
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            forward_to_local_destination<rx_channel_id>(
                local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NEW:
            if constexpr (my_direction == SOUTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SEW:
            if constexpr (my_direction == NORTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NE:
            if constexpr (my_direction == SOUTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_NW:
            if constexpr (my_direction == SOUTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<NORTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SE:
            if constexpr (my_direction == NORTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_east_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<EAST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        case LowLatencyMeshRoutingFields::WRITE_AND_FORWARD_SW:
            if constexpr (my_direction == NORTH) {
                if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                    cached_routing_fields.value++;
                }
                constexpr auto const edm_index = get_downstream_edm_interface_index<SOUTH>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            } else {
                forward_to_local_destination<rx_channel_id>(
                    local_relay_interface, packet_start, payload_size_bytes, transaction_id);
            }
            if constexpr (UPDATE_PKT_HDR_ON_RX_CH) {
                cached_routing_fields.hop_index = cached_routing_fields.branch_west_offset;
            }
            {
                constexpr auto const edm_index = get_downstream_edm_interface_index<WEST>();
                forward_payload_to_downstream_edm<enable_deadlock_avoidance, false, !UPDATE_PKT_HDR_ON_RX_CH>(
                    packet_start,
                    payload_size_bytes,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0[edm_index],
                    transaction_id);
            }
            break;
        default: __builtin_unreachable();
    }
}
#endif

template <typename EdmChannelWorkerIFs>
FORCE_INLINE void establish_edm_connection(
    EdmChannelWorkerIFs & local_sender_channel_worker_interface) {
    local_sender_channel_worker_interface.template cache_producer_noc_addr<ENABLE_RISC_CPU_DATA_CACHE, USE_DYNAMIC_CREDIT_ADDR>();
}

bool any_sender_channels_active(
    std::array<uint32_t, NUM_SENDER_CHANNELS> const& local_sender_channel_free_slots_stream_ids) {
    // TODO implement template meta program structure to unroll loops
    // Manually unroll loop for RV32I optimization - typically 2-8 channels
    {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[0U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[0U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 1U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[1U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[1U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 2U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[2U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[2U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 3U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[3U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[3U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 4U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[4U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[4U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 5U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[5U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[5U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 6U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[6U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[6U])) {
            return true;
        }
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 7U) {
        if (get_ptr_val(local_sender_channel_free_slots_stream_ids[7U]) !=
            static_cast<int32_t>(SENDER_NUM_BUFFERS_ARRAY[7U])) {
            return true;
        }
    }
    
    return false;
}

template <typename LocalTelemetryT>
FORCE_INLINE void update_telemetry(
    std::array<uint32_t, NUM_SENDER_CHANNELS> const& local_sender_channel_free_slots_stream_ids_ordered,
    bool const tx_progress,
    bool const rx_progress,
    LocalTelemetryT& local_fabric_telemetry,
    volatile tt_l1_ptr LocalTelemetryT* fabric_telemetry) {
    if constexpr (FABRIC_TELEMETRY_HEARTBEAT_TX) {
        bool sender_idle = false;
        if (!tx_progress) {
            sender_idle = !any_sender_channels_active(local_sender_channel_free_slots_stream_ids_ordered);
        }
        if (tx_progress || sender_idle) {
            volatile RiscTimestampV2* tx_heartbeat_addr =
                &fabric_telemetry->dynamic_info.erisc[MY_ERISC_ID].tx_heartbeat;
            local_fabric_telemetry.dynamic_info.erisc[MY_ERISC_ID].tx_heartbeat.full++;
            tx_heartbeat_addr->full = local_fabric_telemetry.dynamic_info.erisc[MY_ERISC_ID].tx_heartbeat.full;
        }
    }
    if constexpr (FABRIC_TELEMETRY_HEARTBEAT_RX) {
        bool receiver_idle = false;
        if (!rx_progress) {
            receiver_idle = (get_ptr_val<to_receiver_packets_sent_streams[0]>() == 0);
        }
        if (rx_progress || receiver_idle) {
            volatile RiscTimestampV2* rx_heartbeat_addr =
                &fabric_telemetry->dynamic_info.erisc[MY_ERISC_ID].rx_heartbeat;
            local_fabric_telemetry.dynamic_info.erisc[MY_ERISC_ID].rx_heartbeat.full++;
            rx_heartbeat_addr->full = local_fabric_telemetry.dynamic_info.erisc[MY_ERISC_ID].rx_heartbeat.full;
        }
    }

    if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
        // Helper to safely write to volatile BandwidthTelemetry destinations without discarding qualifiers
        auto store_bandwidth_telemetry = [](volatile BandwidthTelemetry* dst, const BandwidthTelemetry& src) {
            dst->elapsed_active_cycles.full = src.elapsed_active_cycles.full;
            dst->elapsed_cycles.full = src.elapsed_cycles.full;
            dst->num_words_sent = src.num_words_sent;
            dst->num_packets_sent = src.num_packets_sent;
        };

        if constexpr (NUM_ACTIVE_ERISCS == 1) {
            store_bandwidth_telemetry(
                &fabric_telemetry->dynamic_info.tx_bandwidth, local_fabric_telemetry.dynamic_info.tx_bandwidth);
            store_bandwidth_telemetry(
                &fabric_telemetry->dynamic_info.rx_bandwidth, local_fabric_telemetry.dynamic_info.rx_bandwidth);
        } else {
            if constexpr (MY_ERISC_ID == 0) {
                store_bandwidth_telemetry(
                    &fabric_telemetry->dynamic_info.tx_bandwidth, local_fabric_telemetry.dynamic_info.tx_bandwidth);
            } else {
                store_bandwidth_telemetry(
                    &fabric_telemetry->dynamic_info.rx_bandwidth, local_fabric_telemetry.dynamic_info.rx_bandwidth);
            }
        }
    }
}

template <bool enable_deadlock_avoidance, bool SKIP_CONNECTION_LIVENESS_CHECK, typename EdmChannelWorkerIFs>
FORCE_INLINE void send_credits_to_upstream_workers(
    EdmChannelWorkerIFs& local_sender_channel_worker_interface,
    int32_t const num_credits,
    bool const channel_connection_established) {
    if constexpr (SKIP_CONNECTION_LIVENESS_CHECK) {
        local_sender_channel_worker_interface
            .template notify_persistent_connection_of_free_space<enable_deadlock_avoidance>(num_credits);
    } else {
        // Connection liveness checks are only done for connections that are not persistent
        // For those connections, it's unsafe to use free-slots counters held in stream registers
        // due to the lack of race avoidant connection protocol. Therefore, we update our read counter
        // instead because these connections will be read/write counter based instead
        local_sender_channel_worker_interface.increment_local_read_counter(num_credits);
        if (channel_connection_established) {
            local_sender_channel_worker_interface
                .template notify_worker_of_read_counter_update<enable_read_counter_update_noc_flush>();
        } else {
            local_sender_channel_worker_interface.copy_read_counter_to_worker_location_info();
            // If not connected, we update the read counter in L1 as well so the next connecting worker
            // is more likely to see space available as soon as it tries connecting
        }
    }
}

template <typename LocalTelemetryT>
FORCE_INLINE void update_bw_counters(
    size_t const packet_bytes,
    LocalTelemetryT& local_fabric_telemetry) {
    const auto packet_words = (packet_bytes + 3) >> 2;
    if constexpr ((NUM_ACTIVE_ERISCS == 1) || (MY_ERISC_ID == 0)) {
        auto & bw = local_fabric_telemetry.dynamic_info.tx_bandwidth;
        bw.num_packets_sent++;
        bw.num_words_sent += packet_words;
        //local_fabric_telemetry.dynamic_info.tx_bandwidth.num_packets_sent++;
        //local_fabric_telemetry.dynamic_info.tx_bandwidth.num_words_sent += (packet_bytes + 3) >> 2;
    }
    if constexpr ((NUM_ACTIVE_ERISCS == 1) || (MY_ERISC_ID == 1)) {
        auto & bw = local_fabric_telemetry.dynamic_info.rx_bandwidth;
        bw.num_packets_sent++;
        bw.num_words_sent += packet_words;
        //local_fabric_telemetry.dynamic_info.rx_bandwidth.num_packets_sent++;
        //local_fabric_telemetry.dynamic_info.rx_bandwidth.num_words_sent += (packet_bytes + 3) >> 2;
    }
}

template <typename LocalTelemetryT>
FORCE_INLINE void update_bw_cycles(
    uint64_t const loop_delta_cycles, bool const tx_progress, bool const rx_progress, LocalTelemetryT& local_fabric_telemetry) {
    if constexpr ((NUM_ACTIVE_ERISCS == 1) || (MY_ERISC_ID == 0)) {
        local_fabric_telemetry.dynamic_info.tx_bandwidth.elapsed_cycles.full += loop_delta_cycles;
        if (tx_progress) {
            local_fabric_telemetry.dynamic_info.tx_bandwidth.elapsed_active_cycles.full += loop_delta_cycles;
        }
    }
    if constexpr ((NUM_ACTIVE_ERISCS == 1) || (MY_ERISC_ID == 1)) {
        local_fabric_telemetry.dynamic_info.rx_bandwidth.elapsed_cycles.full += loop_delta_cycles;
        if (rx_progress) {
            local_fabric_telemetry.dynamic_info.rx_bandwidth.elapsed_active_cycles.full += loop_delta_cycles;
        }
    }
}

////////////////////////////////////
////////////////////////////////////
//  Main Control Loop
////////////////////////////////////
////////////////////////////////////
template <
    uint8_t sender_channel_index,
    uint8_t to_receiver_pkts_sent_id,
    bool SKIP_CONNECTION_LIVENESS_CHECK,
    typename SenderChannelT,
    typename WorkerInterfaceT,
    typename ReceiverChannelT,
    typename LocalTelemetryT>
FORCE_INLINE bool run_sender_channel_step_impl(
    SenderChannelT& local_sender_channel,
    WorkerInterfaceT& local_sender_channel_worker_interface,
    uint32_t& outbound_to_receiver_channel_pointers_num_free_slots,
    ReceiverChannelT& remote_receiver_channel,
    bool& channel_connection_established,
    uint32_t const sender_channel_free_slots_stream_id,
    SenderChannelFromReceiverCredits& sender_channel_from_receiver_credits,
    PerfTelemetryRecorder& perf_telemetry_recorder,
    LocalTelemetryT& local_fabric_telemetry) {
    // If the receiver has space, and we have one or more packets unsent from producer, then send one
    // TODO: convert to loop to send multiple packets back to back (or support sending multiple packets in one shot)
    //       when moving to stream regs to manage rd/wr ptrs
    // TODO: update to be stream reg based. Initialize to space available and simply check for non-zero

    constexpr bool const use_bubble_flow_control =
        sender_channel_is_traffic_injection_channel[sender_channel_index] && enable_deadlock_avoidance;
    static_assert(
        !use_bubble_flow_control || ENABLE_FIRST_LEVEL_ACK,
        "Bubble flow control and first level ack must be set to the same values"
    );

    uint32_t const free_slots = get_ptr_val(sender_channel_free_slots_stream_id);
    
    bool receiver_has_space_for_packet;
    if constexpr (use_bubble_flow_control) {
        receiver_has_space_for_packet = outbound_to_receiver_channel_pointers_num_free_slots >=
                                        BUBBLE_FLOW_CONTROL_INJECTION_SENDER_CHANNEL_MIN_FREE_SLOTS;
    } else {
        receiver_has_space_for_packet = outbound_to_receiver_channel_pointers_num_free_slots != 0U;
    }
    bool const has_unsent_packet = free_slots < WorkerInterfaceT::num_buffers;
    bool can_send = receiver_has_space_for_packet && has_unsent_packet;

    if constexpr (!ETH_TXQ_SPIN_WAIT_SEND_NEXT_DATA) {
        can_send = can_send && !internal_::eth_txq_is_busy(sender_txq_id);
    }

    if (can_send) {
        did_something = true;

        auto* pkt_header = reinterpret_cast<volatile tt_l1_ptr PACKET_HEADER_TYPE*>(
            local_sender_channel.get_cached_next_buffer_slot_addr());
        // Cache packet size before send to avoid redundant volatile load in telemetry
        size_t packet_size_bytes = 0;
        if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
            packet_size_bytes = pkt_header->get_payload_size_including_header();
        }

        if constexpr (!UPDATE_PKT_HDR_ON_RX_CH) {
            update_packet_header_before_eth_send<sender_channel_index>(pkt_header);
        }
        send_next_data<sender_channel_index, to_receiver_pkts_sent_id, SKIP_CONNECTION_LIVENESS_CHECK>(
            local_sender_channel,
            local_sender_channel_worker_interface,
            outbound_to_receiver_channel_pointers_num_free_slots,
            remote_receiver_channel,
            perf_telemetry_recorder
        );
        // Update local TX counters: split responsibility in multi-ERISC mode
        if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
            update_bw_counters(packet_size_bytes, local_fabric_telemetry);
        }
        increment_local_update_ptr_val(sender_channel_free_slots_stream_id, 1);
    }

    // Process COMPLETIONs from receiver
    int32_t const completions_since_last_check =
        sender_channel_from_receiver_credits.template get_num_unprocessed_completions_from_receiver<ENABLE_RISC_CPU_DATA_CACHE>();
    if (completions_since_last_check) {
        outbound_to_receiver_channel_pointers_num_free_slots += completions_since_last_check;
        sender_channel_from_receiver_credits.increment_num_processed_completions(completions_since_last_check);

        // When first level ack is enabled, then credits can be sent to upstream workers as soon as we see
        // the ack, we don't need to wait for the completion from receiver. Therefore, only when we have
        // first level ack disabled will we send credits to workers on receipt of completion acknowledgements.
        if constexpr (!ENABLE_FIRST_LEVEL_ACK) {
            send_credits_to_upstream_workers<enable_deadlock_avoidance, SKIP_CONNECTION_LIVENESS_CHECK>(
                local_sender_channel_worker_interface, completions_since_last_check, channel_connection_established);
        }
    }

    // Process ACKs from receiver
    // ACKs are processed second to avoid any sort of races. If we process acks second,
    // we are guaranteed to see equal to or greater the number of acks than completions
    if constexpr (ENABLE_FIRST_LEVEL_ACK) {
        auto acks_since_last_check = sender_channel_from_receiver_credits.template get_num_unprocessed_acks_from_receiver<ENABLE_RISC_CPU_DATA_CACHE>();
        if (acks_since_last_check) {
            sender_channel_from_receiver_credits.increment_num_processed_acks(acks_since_last_check);
            send_credits_to_upstream_workers<enable_deadlock_avoidance, SKIP_CONNECTION_LIVENESS_CHECK>(
                local_sender_channel_worker_interface, acks_since_last_check, channel_connection_established);
        }
    }

    if constexpr (!SKIP_CONNECTION_LIVENESS_CHECK) {
        auto const check_connection_status =
            !channel_connection_established || local_sender_channel_worker_interface.has_worker_teardown_request();
        if (check_connection_status) {
            check_worker_connections<MY_ETH_CHANNEL, ENABLE_RISC_CPU_DATA_CACHE>(
                local_sender_channel_worker_interface,
                channel_connection_established,
                sender_channel_free_slots_stream_id);
        }
    }
    return did_something;
};

template <
    uint8_t VC_RECEIVER_CHANNEL,
    uint8_t sender_channel_index,
    typename EthSenderChannels,
    typename EdmChannelWorkerIFs,
    typename RemoteEthReceiverChannels,
    size_t NUM_SENDER_CHANNELS,
    typename LocalTelemetryT>
FORCE_INLINE bool run_sender_channel_step(
    EthSenderChannels& local_sender_channels,
    EdmChannelWorkerIFs& local_sender_channel_worker_interfaces,
    uint32_t& outbound_to_receiver_channel_pointers_num_free_slots,
    RemoteEthReceiverChannels& remote_receiver_channels,
    std::array<bool, NUM_SENDER_CHANNELS>& channel_connection_established,
    std::array<uint32_t, NUM_SENDER_CHANNELS>& local_sender_channel_free_slots_stream_ids,
    std::array<SenderChannelFromReceiverCredits, NUM_SENDER_CHANNELS>& sender_channel_from_receiver_credits,
    PerfTelemetryRecorder& perf_telemetry_recorder,
    LocalTelemetryT& local_fabric_telemetry) {
    if constexpr (is_sender_channel_serviced[sender_channel_index]) {
        // the cache is invalidated here because the channel will read some
        // L1 locations to see if it can make progress
        router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        return run_sender_channel_step_impl<
            sender_channel_index,
            to_receiver_packets_sent_streams[VC_RECEIVER_CHANNEL],
            sender_ch_live_check_skip[sender_channel_index]>(
            local_sender_channels.template get<sender_channel_index>(),
            local_sender_channel_worker_interfaces.template get<sender_channel_index>(),
            outbound_to_receiver_channel_pointers_num_free_slots,
            remote_receiver_channels.template get<VC_RECEIVER_CHANNEL>(),
            channel_connection_established[sender_channel_index],
            local_sender_channel_free_slots_stream_ids[sender_channel_index],
            sender_channel_from_receiver_credits[sender_channel_index],
            perf_telemetry_recorder,
            local_fabric_telemetry);
    }
    return false;
}

template <
    uint8_t receiver_channel,
    uint8_t to_receiver_pkts_sent_id,
    typename WriteTridTracker,
    typename ReceiverChannelBufferT,
    typename ReceiverChannelPointersT,
    typename DownstreamSenderVC0T,
    typename LocalRelayInterfaceT,
    typename LocalTelemetryT>
FORCE_INLINE bool run_receiver_channel_step_impl(
    ReceiverChannelBufferT& local_receiver_channel,
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0>& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT& local_relay_interface,
    ReceiverChannelPointersT& receiver_channel_pointers,
    WriteTridTracker& receiver_channel_trid_tracker,
    ReceiverChannelResponseCreditSender& receiver_channel_response_credit_sender,
    const tt::tt_fabric::routing_l1_info_t& routing_table,
    LocalTelemetryT& local_fabric_telemetry) {

    auto& wr_sent_counter = receiver_channel_pointers.wr_sent_counter;
    auto const pkts_received_since_last_check = get_ptr_val<to_receiver_pkts_sent_id>();

    bool unwritten_packets;
    if constexpr (ENABLE_FIRST_LEVEL_ACK) {
        auto& ack_counter = receiver_channel_pointers.ack_counter;
        bool const pkts_received = pkts_received_since_last_check > 0;
        bool can_send_ack = pkts_received;
        if constexpr (!ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK) {
            can_send_ack = can_send_ack && !internal_::eth_txq_is_busy(receiver_txq_id);
        }
        if (can_send_ack) {
            // currently only support processing one packet at a time, so we only decrement by 1
            router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
            increment_local_update_ptr_val<to_receiver_pkts_sent_id>(-1);

            uint8_t src_ch_id;
            if constexpr (skip_src_ch_id_update) {
                // skip_src_ch_id_update implies something like mux mode is disabled and there is only a single
                // sender channel so we don't dynamically fetch it off the packet header
                src_ch_id = receiver_channel_pointers.get_src_chan_id();
            } else {
                auto const receiver_buffer_index = ack_counter.get_buffer_index();
                tt_l1_ptr PACKET_HEADER_TYPE* packet_header = const_cast<PACKET_HEADER_TYPE*>(
                    local_receiver_channel.template get_packet_header<PACKET_HEADER_TYPE>(receiver_buffer_index));
                auto const packet_header_src_ch_id = packet_header->src_ch_id;
                receiver_channel_pointers.set_src_chan_id(receiver_buffer_index, packet_header_src_ch_id);
                src_ch_id = receiver_channel_pointers.get_src_chan_id(receiver_buffer_index);
            }

            receiver_send_received_ack<ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK>(
                receiver_channel_response_credit_sender, src_ch_id);
            ack_counter.increment();
        }
        unwritten_packets = !wr_sent_counter.is_caught_up_to(ack_counter);

    } else {
        unwritten_packets = pkts_received_since_last_check != 0;
    }

    // Code profiling timer for receiver channel forward
    alignas(sizeof(uint32_t)) NamedProfiler<CodeProfilingTimerType::RECEIVER_CHANNEL_FORWARD, code_profiling_enabled_timers_bitfield, code_profiling_buffer_base_addr> receiver_forward_timer;
    receiver_forward_timer.set_should_dump(unwritten_packets);
    receiver_forward_timer.open();

    if (unwritten_packets) {
        router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        auto const receiver_buffer_index = wr_sent_counter.get_buffer_index();
        tt_l1_ptr PACKET_HEADER_TYPE* packet_header = const_cast<PACKET_HEADER_TYPE*>(
            local_receiver_channel.template get_packet_header<PACKET_HEADER_TYPE>(receiver_buffer_index));

        ROUTING_FIELDS_TYPE cached_routing_fields;
#if !defined(FABRIC_2D) || !defined(DYNAMIC_ROUTING_ENABLED)
        cached_routing_fields = packet_header->routing_fields;
#endif
        if constexpr (!skip_src_ch_id_update && !ENABLE_FIRST_LEVEL_ACK) {
            auto const packet_header_src_ch_id = packet_header->src_ch_id;
            receiver_channel_pointers.set_src_chan_id(receiver_buffer_index, packet_header_src_ch_id);
        }
#if defined(FABRIC_2D)
        uint32_t hop_cmd;
#endif        
        bool can_send_to_all_local_chip_receivers;
        if constexpr (is_2d_fabric) {
            // read in the hop command from route buffer.
            // Hop command is 4 bits. Each of the 4 bits signal one of the 4 possible outcomes for a packet.
            // [0]->Forward East
            // [1]->Forward West
            // [2]->Forward North
            // [3]->Forward South
            // The hop command (4-bits) gets decoded as a local write and/or forward to the "other" 3 directions.
            // Other 3 directions depend on the direction of fabric router.
            // For example, a router that is connected West can write locally or forard East, North or South.
            // A local write is encoded by setting the bit corresponding to fabric router's own direction to 1.
            // For a West facing fabric router:
            //  - Hop command of [0010] instructs fabric router to write the packet locally.
            //  - Hop command of [0011] instructs fabric router to write the packet locally AND forward East (a line
            //  mcast)
#if defined(FABRIC_2D)
            // need this ifdef since the packet header for 1D does not have router_buffer field in it.
            hop_cmd = get_cmd_with_mesh_boundary_adjustment(packet_header, cached_routing_fields, routing_table);
            can_send_to_all_local_chip_receivers =
                can_forward_packet_completely(hop_cmd, downstream_edm_interfaces_vc0, local_relay_interface);
#endif
        } else {
#ifndef FABRIC_2D
            can_send_to_all_local_chip_receivers =
                can_forward_packet_completely(cached_routing_fields, downstream_edm_interfaces_vc0[receiver_channel]);
#endif
        }
        if constexpr (enable_trid_flush_check_on_noc_txn) {
            bool const trid_flushed = receiver_channel_trid_tracker.transaction_flushed(receiver_buffer_index);
            can_send_to_all_local_chip_receivers &= trid_flushed;
        }
        if (can_send_to_all_local_chip_receivers) {
            size_t packet_size_bytes = 0;
            if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
                packet_size_bytes = packet_header->get_payload_size_including_header();
            }
            did_something = true;
            // Count RX bytes/packets (header + payload) when consuming a packet from receiver buffer
            if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
                update_bw_counters(packet_size_bytes, local_fabric_telemetry);
            }
            uint8_t const trid = receiver_channel_trid_tracker.update_buffer_slot_to_next_trid_and_advance_trid_counter(
                receiver_buffer_index);
            if constexpr (is_2d_fabric) {
#if defined(FABRIC_2D)
                receiver_forward_packet<receiver_channel>(
                    packet_header,
                    cached_routing_fields,
                    downstream_edm_interfaces_vc0,
                    local_relay_interface,
                    trid,
                    hop_cmd);
#endif
            } else {
#ifndef FABRIC_2D
                receiver_forward_packet<receiver_channel>(
                    packet_header, cached_routing_fields, downstream_edm_interfaces_vc0[0], trid);
#endif
            }
            wr_sent_counter.increment();
            // decrement the to_receiver_pkts_sent_id stream register by 1 since current packet has been processed.
            if constexpr (!ENABLE_FIRST_LEVEL_ACK) {
                increment_local_update_ptr_val<to_receiver_pkts_sent_id>(-1);
            }
        }
    }

    // Close the code profiling timer
    receiver_forward_timer.close();

    if constexpr (!fuse_receiver_flush_and_completion_ptr) {
        auto& wr_flush_counter = receiver_channel_pointers.wr_flush_counter;
        bool const unflushed_writes = !wr_flush_counter.is_caught_up_to(wr_sent_counter);
        if (unflushed_writes) {
            auto const receiver_buffer_index = wr_flush_counter.get_buffer_index();
            bool const next_trid_flushed = receiver_channel_trid_tracker.transaction_flushed(receiver_buffer_index);
            if (next_trid_flushed) {
                wr_flush_counter.increment();
                receiver_channel_trid_tracker.clear_trid_at_buffer_slot(receiver_buffer_index);
            }
        }

        auto& completion_counter = receiver_channel_pointers.completion_counter;
        bool unsent_completions = !completion_counter.is_caught_up_to(completion_counter, wr_flush_counter);
        if constexpr (!ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK) {
            unsent_completions = unsent_completions && !internal_::eth_txq_is_busy(receiver_txq_id);
        }
        if (unsent_completions) {
            // completion ptr incremented in callee
            auto const receiver_buffer_index = wr_flush_counter.get_buffer_index();
            receiver_send_completion_ack<ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK>(
                receiver_channel_response_credit_sender,
                receiver_channel_pointers.get_src_chan_id(receiver_buffer_index));
            completion_counter.increment();
        }
    } else {
        // flush and completion are fused, so we only need to update one of the counters
        // update completion since other parts of the code check against completion
        auto& completion_counter = receiver_channel_pointers.completion_counter;
        // Currently unclear if it's better to loop here or not...
        bool const unflushed_writes = !completion_counter.is_caught_up_to(wr_sent_counter);
        auto const receiver_buffer_index = completion_counter.get_buffer_index();
        bool const next_trid_flushed = receiver_channel_trid_tracker.transaction_flushed(receiver_buffer_index);
        bool can_send_completion = unflushed_writes && next_trid_flushed;
        if constexpr (!ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK) {
            can_send_completion = can_send_completion && !internal_::eth_txq_is_busy(receiver_txq_id);
        }
        if (can_send_completion) {
            uint8_t src_ch_id;
            if constexpr (skip_src_ch_id_update) {
                src_ch_id = receiver_channel_pointers.get_src_chan_id();
            } else {
                src_ch_id = receiver_channel_pointers.get_src_chan_id(receiver_buffer_index);
            }
            receiver_send_completion_ack<ETH_TXQ_SPIN_WAIT_RECEIVER_SEND_COMPLETION_ACK>(
                receiver_channel_response_credit_sender, src_ch_id);
            receiver_channel_trid_tracker.clear_trid_at_buffer_slot(receiver_buffer_index);
            completion_counter.increment();
        }
    }
    return did_something;
};

template <
    uint8_t receiver_channel,
    typename DownstreamSenderVC0T,
    typename LocalRelayInterfaceT,
    typename EthReceiverChannels,
    typename WriteTridTracker,
    typename ReceiverChannelPointersT,
    typename LocalTelemetryT>
FORCE_INLINE bool run_receiver_channel_step(
    EthReceiverChannels& local_receiver_channels,
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0>& downstream_edm_interfaces_vc0,
    LocalRelayInterfaceT& local_relay_interface,
    ReceiverChannelPointersT& receiver_channel_pointers,
    WriteTridTracker& receiver_channel_trid_tracker,
    std::array<ReceiverChannelResponseCreditSender, NUM_RECEIVER_CHANNELS>& receiver_channel_response_credit_senders,
    const tt::tt_fabric::routing_l1_info_t& routing_table,
    LocalTelemetryT& local_fabric_telemetry) {
    if constexpr (is_receiver_channel_serviced[receiver_channel]) {
        router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        return run_receiver_channel_step_impl<
            receiver_channel,
            to_receiver_packets_sent_streams[receiver_channel],
            WriteTridTracker,
            decltype(local_receiver_channels.template get<receiver_channel>()),
            ReceiverChannelPointersT,
            DownstreamSenderVC0T,
            LocalRelayInterfaceT>(
            local_receiver_channels.template get<receiver_channel>(),
            downstream_edm_interfaces_vc0,
            local_relay_interface,
            receiver_channel_pointers,
            receiver_channel_trid_tracker,
            receiver_channel_response_credit_senders[receiver_channel],
            routing_table,
            local_fabric_telemetry);
    }
    return false;
}

/*
 * Main control loop for fabric EDM. Run indefinitely until a termination signal is received
 *
 * Every loop iteration visit a sender channel and the receiver channel. Switch between sender
 * channels every iteration unless it is unsafe/undesirable to do so (e.g. for performance reasons).
 */
template <
    size_t NUM_RECEIVER_CHANNELS,
    typename DownstreamSenderVC0T,
    typename LocalRelayInterfaceT,
    size_t NUM_SENDER_CHANNELS,
    typename EthSenderChannels,
    typename EthReceiverChannels,
    typename RemoteEthReceiverChannels,
    typename EdmChannelWorkerIFs,
    typename TransactionIdTrackerCH0>
FORCE_INLINE void run_fabric_edm_main_loop(
    EthReceiverChannels& local_receiver_channels,
    EthSenderChannels& local_sender_channels,
    EdmChannelWorkerIFs& local_sender_channel_worker_interfaces,
    std::array<DownstreamSenderVC0T, NUM_DOWNSTREAM_SENDERS_VC0>& downstream_edm_noc_interfaces_vc0,
    LocalRelayInterfaceT& local_relay_interface,
    RemoteEthReceiverChannels& remote_receiver_channels,
    volatile tt::tt_fabric::TerminationSignal* termination_signal_ptr,
    TransactionIdTrackerCH0& receiver_channel_0_trid_tracker,
    std::array<uint32_t, NUM_SENDER_CHANNELS>& local_sender_channel_free_slots_stream_ids) {
    size_t did_nothing_count = 0;
    using FabricTelemetryT = FabricTelemetry;
    FabricTelemetryT local_fabric_telemetry{};

    union {
        volatile FabricTelemetryT* dst;
        decltype(MEM_AERISC_FABRIC_TELEMETRY_BASE) src;
    } fabric_telemetry_cast;
    fabric_telemetry_cast.src = MEM_AERISC_FABRIC_TELEMETRY_BASE;
    auto fabric_telemetry = fabric_telemetry_cast.dst;
    
    *termination_signal_ptr = tt::tt_fabric::TerminationSignal::KEEP_RUNNING;

    union {
        tt_l1_ptr tt::tt_fabric::routing_l1_info_t* dst;
        decltype(ROUTING_TABLE_BASE) src;
    } routing_table_l1_cast;
    routing_table_l1_cast.src = ROUTING_TABLE_BASE;
    const auto* routing_table_l1 = routing_table_l1_cast.dst;
    tt::tt_fabric::routing_l1_info_t routing_table = *routing_table_l1;

    // May want to promote to part of the handshake but for now we just initialize in this standalone way
    // TODO: flatten all of these arrays into a single object (one array lookup) OR
    //       (probably better) pack most of these into single words (e.g. we could hold a read, write, and ackptr in a
    //       single word) this way - especially if power of 2 wraps, we can handle both channels literally at once with
    //       math ops on single individual words (or half words)
    auto outbound_to_receiver_channel_pointers =
        ChannelPointersTuple<OutboundReceiverChannelPointers, REMOTE_RECEIVER_NUM_BUFFERS_ARRAY>::make();
    // Workaround the perf regression in RingAsLinear test.
      uint32_t& outbound_to_receiver_channel_pointer_ch0_num_free_slots =
        outbound_to_receiver_channel_pointers.template get<VC0_RECEIVER_CHANNEL>().num_free_slots;

    auto receiver_channel_pointers = ChannelPointersTuple<ReceiverChannelPointers, RECEIVER_NUM_BUFFERS_ARRAY>::make();
    // Workaround the perf regression in RingAsLinear test.
    auto receiver_channel_pointers_ch0 = receiver_channel_pointers.template get<0>();
    receiver_channel_pointers_ch0.reset();
    if constexpr (skip_src_ch_id_update) {
        receiver_channel_pointers_ch0.set_src_chan_id(BufferIndex{0}, remote_worker_sender_channel);
    }

    std::array<bool, NUM_SENDER_CHANNELS> channel_connection_established =
        initialize_array<NUM_SENDER_CHANNELS, bool, false>();

    PerfTelemetryRecorder inner_loop_perf_telemetry_collector =
        build_perf_telemetry_recorder<perf_telemetry_mode>();

    L1PerfTelemetrySingleBuffer local_perf_telemetry_buffer(nullptr);
    if constexpr (is_sender_channel_serviced[0] && perf_telemetry_mode != PerfTelemetryRecorderType::NONE) {
        local_perf_telemetry_buffer =
            build_perf_telemetry_buffer(reinterpret_cast<uint32_t*>(perf_telemetry_buffer_addr));    
    }

    auto receiver_channel_response_credit_senders =
        init_receiver_channel_response_credit_senders<NUM_RECEIVER_CHANNELS>();
    auto sender_channel_from_receiver_credits =
        init_sender_channel_from_receiver_credits_flow_controllers<NUM_SENDER_CHANNELS>();
    // This value defines the number of loop iterations we perform of the main control sequence before exiting
    // to check for termination and context switch. Removing the these checks from the inner loop can drastically
    // improve performance. The value of 32 was chosen somewhat empirically and then raised up slightly.

    uint64_t loop_start_cycles;
    while (!got_immediate_termination_signal<ENABLE_RISC_CPU_DATA_CACHE>(termination_signal_ptr)) {
        did_something = false;

        uint32_t tx_progress = 0;
        uint32_t rx_progress = 0;
        if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
            loop_start_cycles = get_timestamp();
        }

        if constexpr (is_sender_channel_serviced[0U]) {
            open_perf_recording_window(inner_loop_perf_telemetry_collector);
        }
        
        for (size_t i = 0; i < iterations_between_ctx_switch_and_teardown_checks; i++) {
            router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
            // Capture these to see if we made progress

            // There are some cases, mainly for performance, where we don't want to switch between sender channels
            // so we interoduce this to provide finer grain control over when we disable the automatic switching
            tx_progress |= run_sender_channel_step<VC0_RECEIVER_CHANNEL, 0U>(
                local_sender_channels,
                local_sender_channel_worker_interfaces,
                outbound_to_receiver_channel_pointer_ch0_num_free_slots,
                remote_receiver_channels,
                channel_connection_established,
                local_sender_channel_free_slots_stream_ids,
                sender_channel_from_receiver_credits,
                inner_loop_perf_telemetry_collector,
                local_fabric_telemetry);
            rx_progress |= run_receiver_channel_step<0, DownstreamSenderVC0T, decltype(local_relay_interface)>(
                local_receiver_channels,
                downstream_edm_noc_interfaces_vc0,
                local_relay_interface,
                receiver_channel_pointers_ch0,
                receiver_channel_0_trid_tracker,
                receiver_channel_response_credit_senders,
                routing_table,
                local_fabric_telemetry);
            tx_progress |= run_sender_channel_step<VC0_RECEIVER_CHANNEL, 1U>(
                local_sender_channels,
                local_sender_channel_worker_interfaces,
                outbound_to_receiver_channel_pointer_ch0_num_free_slots,
                remote_receiver_channels,
                channel_connection_established,
                local_sender_channel_free_slots_stream_ids,
                sender_channel_from_receiver_credits,
                inner_loop_perf_telemetry_collector,
                local_fabric_telemetry);
#if defined(FABRIC_2D)
            if constexpr (is_2d_fabric) {
                tx_progress = tx_progress | run_sender_channel_step<VC0_RECEIVER_CHANNEL, 2U>(
                    local_sender_channels,
                    local_sender_channel_worker_interfaces,
                    outbound_to_receiver_channel_pointer_ch0_num_free_slots,
                    remote_receiver_channels,
                    channel_connection_established,
                    local_sender_channel_free_slots_stream_ids,
                    sender_channel_from_receiver_credits,
                    inner_loop_perf_telemetry_collector,
                    local_fabric_telemetry)
                | run_sender_channel_step<VC0_RECEIVER_CHANNEL, 3U>(
                    local_sender_channels,
                    local_sender_channel_worker_interfaces,
                    outbound_to_receiver_channel_pointer_ch0_num_free_slots,
                    remote_receiver_channels,
                    channel_connection_established,
                    local_sender_channel_free_slots_stream_ids,
                    sender_channel_from_receiver_credits,
                    inner_loop_perf_telemetry_collector,
                    local_fabric_telemetry);
            }
#endif            
        }

        // Compute idle conditions and update heartbeats in one helper
        if constexpr (FABRIC_TELEMETRY_ANY_DYNAMIC_STAT) {
            if constexpr (FABRIC_TELEMETRY_BANDWIDTH) {
                const uint64_t loop_end_cycles = get_timestamp();
                const uint64_t loop_delta_cycles = loop_end_cycles - loop_start_cycles;
                update_bw_cycles(loop_delta_cycles, tx_progress, rx_progress, local_fabric_telemetry);
            }
            update_telemetry(
                local_sender_channel_free_slots_stream_ids,
                tx_progress,
                rx_progress,
                local_fabric_telemetry,
                fabric_telemetry);
        }

        if constexpr (enable_context_switch) {
            // shouldn't do noc counter sync since we are not incrementing them
            if constexpr (IDLE_CONTEXT_SWITCHING) {
                if (did_something) {
                    did_nothing_count = 0U;
                } else {
                    if (did_nothing_count++ > SWITCH_INTERVAL) {
                        did_nothing_count = 0U;
                        run_routing_without_noc_sync();
                    }
                }
            } else {
                if (did_nothing_count++ > SWITCH_INTERVAL) {
                    did_nothing_count = 0U;
                    run_routing_without_noc_sync();
                }
            }
        }

        if constexpr (is_sender_channel_serviced[0U]) {
            close_perf_recording_window(inner_loop_perf_telemetry_collector);
            if constexpr (perf_telemetry_mode != PerfTelemetryRecorderType::NONE) {
                if (captured_an_event(inner_loop_perf_telemetry_collector) ||
                    any_sender_channels_active(local_sender_channel_free_slots_stream_ids)) {
                    write_perf_recording_window_results(
                        inner_loop_perf_telemetry_collector, local_perf_telemetry_buffer);
                }
            }
        }
    }
}

template <typename EdmChannelWorkerIFs, size_t NUM_SENDER_CHANNELS>
void
#ifdef FABRIC_2D
    __attribute__((noinline))
#endif
    wait_for_static_connection_to_ready(
        EdmChannelWorkerIFs & local_sender_channel_worker_interfaces,
        std::array<uint32_t, NUM_SENDER_CHANNELS> & local_sender_channel_free_slots_stream_ids) {
    auto establish_static_connection_from_receiver_side = [&](auto& interface, size_t const sender_channel_idx) {
        if (!sender_ch_live_check_skip[sender_channel_idx]) {
            return;
        }
        while (!connect_is_requested(*interface.connection_live_semaphore)) {
            router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        }
        establish_edm_connection(interface); //, local_sender_channel_free_slots_stream_ids[sender_channel_idx]);
    };
    if constexpr (multi_txq_enabled) {
        array_like_for_each_constexpr<NUM_SENDER_CHANNELS>(
            local_sender_channel_worker_interfaces, [&](auto& interface, auto const idx) {
                if constexpr (is_sender_channel_serviced[idx]) {
                    establish_static_connection_from_receiver_side(interface, idx);
                }
            });
    } else {
        // Very slight performance regression on WH if we commonize to the above path, so we preserve this path
        // too
        array_like_for_each<NUM_SENDER_CHANNELS>(
            local_sender_channel_worker_interfaces,
            [&](auto& interface, size_t const idx) { establish_static_connection_from_receiver_side(interface, idx); });
    }
}

// Returns the number of starting credits for the specified sender channel `i`
// Generally, we will always start with `SENDER_NUM_BUFFERS` of credits,
// except for channels which service transient/worker connections. Those
// sender channels use counter based credit schemes so they are initialized
// to 0.
template <size_t I>
constexpr size_t get_credits_init_val() {
    return std::conditional_t<
        I == 0U,
        std::integral_constant<size_t, 0U>,
        std::integral_constant<size_t, SENDER_NUM_BUFFERS_ARRAY[I]>
    >::value;
};

// SFINAE helper to initialize a single sender channel worker interface
// Only enabled when I < NUM_SENDER_CHANNELS
template <size_t I, size_t NUM_SENDER_CHANNELS, typename EdmChannelWorkerIFs>
FORCE_INLINE typename std::enable_if<(I < NUM_SENDER_CHANNELS), void>::type init_sender_channel_worker_interface(
    std::array<size_t, NUM_SENDER_CHANNELS>& local_sender_connection_live_semaphore_addresses,
    std::array<size_t, NUM_SENDER_CHANNELS>& local_sender_connection_info_addresses,
    EdmChannelWorkerIFs& local_sender_channel_worker_interfaces) {
    auto connection_live_semaphore_ptr =
        reinterpret_cast<volatile tt_l1_ptr uint32_t* const>(local_sender_connection_live_semaphore_addresses[I]);
    auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
        local_sender_connection_info_addresses[I]);
    new (&local_sender_channel_worker_interfaces.template get<I>()) tt::tt_fabric::
        StaticSizedSenderChannelWorkerInterface<tt::tt_fabric::worker_handshake_noc, SENDER_NUM_BUFFERS_ARRAY[I]>(
            connection_worker_info_ptr,
            0U,  // Not used for credits.
            reinterpret_cast<volatile tt_l1_ptr uint32_t* const>(connection_live_semaphore_ptr),
            sender_channel_ack_cmd_buf_ids[I],
            get_credits_init_val<I>(),
            notify_worker_of_read_counter_update_src_address);
}

// SFINAE overload - no-op when I >= NUM_SENDER_CHANNELS
template <size_t I, size_t NUM_SENDER_CHANNELS, typename EdmChannelWorkerIFs>
typename std::enable_if<(I >= NUM_SENDER_CHANNELS), void>::type init_sender_channel_worker_interface(
    std::array<size_t, NUM_SENDER_CHANNELS>&, std::array<size_t, NUM_SENDER_CHANNELS>&, EdmChannelWorkerIFs&) {
    // No-op when channel index is out of range
}

template <size_t NUM_SENDER_CHANNELS, typename EdmChannelWorkerIFs>
void
#ifdef FABRIC_2D
    __attribute__((noinline))
#endif
    init_local_sender_channel_worker_interfaces(
        std::array<size_t, NUM_SENDER_CHANNELS>& local_sender_connection_live_semaphore_addresses,
        std::array<size_t, NUM_SENDER_CHANNELS>& local_sender_connection_info_addresses,
        EdmChannelWorkerIFs& local_sender_channel_worker_interfaces) {
    // manual unrol because previously, going from having this in a loop to unrolling this would
    // lead to a performance regression. Having these unrolled is needed to enable some performance optimizations
    // because setup will differ in that each will be a different type. Keeping them unrolled here let's us
    // stay safe from perf regression due to weirdness of codegen.
    init_sender_channel_worker_interface<0, NUM_SENDER_CHANNELS>(
        local_sender_connection_live_semaphore_addresses,
        local_sender_connection_info_addresses,
        local_sender_channel_worker_interfaces);
    if constexpr (NUM_SENDER_CHANNELS > 1U) {
        init_sender_channel_worker_interface<1U, NUM_SENDER_CHANNELS>(
            local_sender_connection_live_semaphore_addresses,
            local_sender_connection_info_addresses,
            local_sender_channel_worker_interfaces);
    }
#ifdef FABRIC_2D
    if constexpr (NUM_SENDER_CHANNELS > 2U) {
        init_sender_channel_worker_interface<2U, NUM_SENDER_CHANNELS>(
            local_sender_connection_live_semaphore_addresses,
            local_sender_connection_info_addresses,
            local_sender_channel_worker_interfaces);
    }
    if constexpr (NUM_SENDER_CHANNELS > 3U) {
        init_sender_channel_worker_interface<3U, NUM_SENDER_CHANNELS>(
            local_sender_connection_live_semaphore_addresses,
            local_sender_connection_info_addresses,
            local_sender_channel_worker_interfaces);
    }
#endif
}

// copy the sender_channel_free_slots_stream_ids (in L1) to local memory for performance.
template <size_t NUM_SENDER_CHANNELS>
FORCE_INLINE void populate_local_sender_channel_free_slots_stream_id_ordered_map(
    std::array<uint32_t, NUM_SENDER_CHANNELS>& local_sender_channel_free_slots_stream_ids) {
    std::copy_n(
        sender_channel_free_slots_stream_ids.begin(),
        NUM_SENDER_CHANNELS,
        local_sender_channel_free_slots_stream_ids.begin()
    );
}

constexpr bool IS_TEARDOWN_MASTER() { return MY_ERISC_ID == 0U; }

FORCE_INLINE void wait_for_other_local_erisc() {
    constexpr uint32_t const multi_erisc_sync_start_value = 0x0fedU;
    constexpr uint32_t const multi_erisc_sync_step2_value = 0x1badU;
    if constexpr (IS_TEARDOWN_MASTER()) {
        write_stream_scratch_register<MULTI_RISC_TEARDOWN_SYNC_STREAM_ID>(multi_erisc_sync_start_value);
        while ((read_stream_scratch_register<MULTI_RISC_TEARDOWN_SYNC_STREAM_ID>() & 0x1FFFU) !=
               multi_erisc_sync_step2_value) {
            router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        }
        write_stream_scratch_register<MULTI_RISC_TEARDOWN_SYNC_STREAM_ID>(0U);
    } else {
        while ((read_stream_scratch_register<MULTI_RISC_TEARDOWN_SYNC_STREAM_ID>() & 0x1FFFU) !=
               multi_erisc_sync_start_value) {
            router_invalidate_l1_cache<ENABLE_RISC_CPU_DATA_CACHE>();
        }
        write_stream_scratch_register<MULTI_RISC_TEARDOWN_SYNC_STREAM_ID>(multi_erisc_sync_step2_value);
    }
}

FORCE_INLINE void teardown(
    volatile tt_l1_ptr tt::tt_fabric::TerminationSignal* termination_signal_ptr,
    volatile tt_l1_ptr tt::tt_fabric::EDMStatus* edm_status_ptr,
    WriteTransactionIdTracker<
        RECEIVER_NUM_BUFFERS_ARRAY[0U],
        NUM_TRANSACTION_IDS,
        0U,
        edm_to_local_chip_noc,
        edm_to_downstream_noc> receiver_channel_0_trid_tracker) {
    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        wait_for_other_local_erisc();
    }
    if constexpr (is_receiver_channel_serviced[0U]) {
        receiver_channel_0_trid_tracker.all_buffer_slot_transactions_acked();
    }

    // at minimum, the below call must be updated because in dynamic noc mode, the counters would be shared, so you'd
    // want a sync before this and coordination about which erisc should do the reset (only one of them should do it)
    static_assert(
        noc_mode != DM_DYNAMIC_NOC,
        "The fabric router implementation doesn't support dynamic noc mode. The implementation must be updated to "
        "support this");
    // re-init the noc counters as the noc api used is not incrementing them
    ncrisc_noc_counters_init();

    if constexpr (NUM_ACTIVE_ERISCS > 1) {
        wait_for_other_local_erisc();
    }
    if constexpr (wait_for_host_signal) {
        if constexpr (is_local_handshake_master) {
            notify_subordinate_routers(
                edm_channels_mask,
                local_handshake_master_eth_chan,
                (uint32_t)termination_signal_ptr,
                *termination_signal_ptr);
        }
    }

    // write barrier should be coordinated for dynamic noc mode. Safest is probably to do a `wait_for_other_local_erisc`
    // followed by master core doing barrier
    static_assert(noc_mode != DM_DYNAMIC_NOC, "Update here when enabling dynamic noc mode");
    noc_async_write_barrier();
    noc_async_atomic_barrier();

    if constexpr (NUM_ACTIVE_ERISCS > 1) {
        wait_for_other_local_erisc();
    }
    if constexpr (IS_TEARDOWN_MASTER()) {
        *edm_status_ptr = tt::tt_fabric::EDMStatus::TERMINATED;
    }
}

void initialize_state_for_txq1_active_mode() {
    eth_enable_packet_mode(receiver_txq_id);

    // TODO implement using template meta pogramming structure
    // Manually unroll loop for RV32I optimization - typically 1-2 channels
    reinterpret_cast<volatile uint32_t*>(local_receiver_ack_counters_base_address)[0U] = 0U;
    reinterpret_cast<volatile uint32_t*>(local_receiver_completion_counters_base_address)[0U] = 0U;
    
    if constexpr (NUM_RECEIVER_CHANNELS > 1U) {
        reinterpret_cast<volatile uint32_t*>(local_receiver_ack_counters_base_address)[1U] = 0U;
        reinterpret_cast<volatile uint32_t*>(local_receiver_completion_counters_base_address)[1U] = 0U;
    }

    eth_txq_reg_write(receiver_txq_id, ETH_TXQ_DATA_PACKET_ACCEPT_AHEAD, DEFAULT_NUM_ETH_TXQ_DATA_PACKET_ACCEPT_AHEAD);
}

FORCE_INLINE void initialize_state_for_txq1_active_mode_sender_side() {
    // TODO implement using template meta pogramming structure
    // Manually unroll loop for RV32I optimization - typically 2-8 channels
    reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[0U] = 0U;
    reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[0U] = 0U;
    
    if constexpr (NUM_SENDER_CHANNELS > 1U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[1U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[1U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 2U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[2U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[2U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 3U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[3U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[3U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 4U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[4U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[4U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 5U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[5U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[5U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 6U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[6U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[6U] = 0U;
    }
    
    if constexpr (NUM_SENDER_CHANNELS > 7U) {
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_ack_counters_base_address)[7U] = 0U;
        reinterpret_cast<volatile uint32_t*>(to_sender_remote_completion_counters_base_address)[7U] = 0U;
    }
}

#if defined(FABRIC_2D)
    #define DOWNSTREAM_EDM_VC0_WORKER_REGISTRATION_ID 0U
    #define DOWNSTREAM_EDM_VC0_WORKER_LOCATION_INFO_ADDRESSES 1U
    #define DOWNSTREAM_EDM_VC0_WORKER_INDEX_SEMAPHORE_ADDRESSES 2U
    #define DOWNSTREAM_EDM_VC0_BUFFER_BASE_ADDRESSES 3U
    #define DOWNSTREAM_EDM_VC0_NUM_FIELDS 4U

    // data type layout and memory alignment of data structures has a noticable impact on performance,
    // keeping values nearby in memory helps with prefeteching.
    //
    alignas(sizeof(uint32_t)) static uint32_t downstream_edm_vc0_worker[NUM_DOWNSTREAM_SENDERS_VC0][DOWNSTREAM_EDM_VC0_NUM_FIELDS];
#endif

struct local_tensix_relay_empty_type {
};

struct alignas(sizeof(uint32_t)) local_tensix_relay_impl_type {
    uint32_t buffer_base_address;
    uint32_t noc_x;
    uint32_t noc_y;
    uint32_t worker_registration_id;
    uint32_t worker_location_info_address;
    uint32_t free_slots_stream_id;
    uint32_t connection_buffer_index_id;
    // unused padding to make size multiple of 8 bytes
    const uint32_t padding = 0U;

    local_tensix_relay_impl_type() :
        buffer_base_address(0U),
        noc_x(0U),
        noc_y(0U),
        worker_registration_id(0U),
        worker_location_info_address(0U),
        free_slots_stream_id(0U),
        connection_buffer_index_id(0U),
        padding(0U) {}
};

template<bool UMD_MODE>
using local_tensix_relay_cond_t = std::conditional_t<
    UMD_MODE,
    local_tensix_relay_impl_type,
    local_tensix_relay_empty_type
>;

struct RouterToRouterSenderEmpty {};

template<bool UMD_MODE>
using RouterToRouterSender_t = std::conditional_t<
    UMD_MODE,
    RouterToRouterSender<LOCAL_RELAY_NUM_BUFFERS>,
    RouterToRouterSenderEmpty
>;

// runs prior to kernel_main
//
__attribute__((constructor)) void kernel_main_ini() {
    set_l1_data_cache<ENABLE_RISC_CPU_DATA_CACHE>();        
}

void kernel_main() {
    eth_txq_reg_write(sender_txq_id, ETH_TXQ_DATA_PACKET_ACCEPT_AHEAD, DEFAULT_NUM_ETH_TXQ_DATA_PACKET_ACCEPT_AHEAD);

    static_assert(
        receiver_txq_id == sender_txq_id || receiver_txq_id == 1U,
        "For multi-txq mode, the only currently supported configuration is sender_txq_id=0 and receiver_txq_id=1");
        
    if constexpr (receiver_txq_id != sender_txq_id) {
        constexpr bool const is_erisc_that_sets_up_second_txq =
            is_receiver_channel_serviced[0U];
        if constexpr (is_erisc_that_sets_up_second_txq) {
            initialize_state_for_txq1_active_mode();
        }
        if constexpr (is_sender_channel_serviced[0U]) {
            initialize_state_for_txq1_active_mode_sender_side();
        }
    }

    //
    // COMMON CT ARGS (not specific to sender or receiver)
    //

    // Initialize stream register state for credit management across the Ethernet link.
    // We make sure to do this before we handshake to guarantee that the registers are
    // initialized before the other side has any possibility of modifying them.
    init_ptr_val<to_receiver_packets_sent_streams[0U]>(0U);
    init_ptr_val<to_sender_packets_acked_streams[0U]>(0U);
    init_ptr_val<to_sender_packets_acked_streams[1U]>(0U);
    init_ptr_val<to_sender_packets_completed_streams[0U]>(0U);
    init_ptr_val<to_sender_packets_completed_streams[1U]>(0U);
    // The first sender channel in the array is always for the transient/worker connection
    init_ptr_val<sender_channel_free_slots_stream_ids[0U]>(SENDER_NUM_BUFFERS_ARRAY[0U]);  // LOCAL WORKER
    init_ptr_val<sender_channel_free_slots_stream_ids[1U]>(SENDER_NUM_BUFFERS_ARRAY[1U]);  // Compact index 0

    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        wait_for_other_local_erisc();
    }

    if constexpr (is_2d_fabric) {
        init_ptr_val<to_receiver_packets_sent_streams[1U]>(0U);
        init_ptr_val<to_sender_packets_acked_streams[2U]>(0U);
        init_ptr_val<to_sender_packets_acked_streams[3U]>(0U);

        // Initialize completion streams and sender channel free slots for channels 2-7 using compile-time loop
        // SENDER_NUM_BUFFERS_ARRAY[] is sized to NUM_SENDER_CHANNELS, which is the number of used sender channels.
        [&]<size_t... Is>(std::index_sequence<Is...>) {
            (([&]() {
                 init_ptr_val<to_sender_packets_completed_streams[Is + 2U]>(0U);
                 if constexpr (NUM_SENDER_CHANNELS > (Is + 2U)) {
                     init_ptr_val<sender_channel_free_slots_stream_ids[Is + 2U]>(SENDER_NUM_BUFFERS_ARRAY[Is + 2U]);
                 }
             }()),
             ...);
        }(std::make_index_sequence<6U>{});
    }

    if constexpr (code_profiling_enabled_timers_bitfield != 0U) {
        clear_code_profiling_buffer(code_profiling_buffer_base_addr);
    }

    // TODO: CONVERT TO SEMAPHORE
    volatile auto termination_signal_ptr =
        reinterpret_cast<volatile tt::tt_fabric::TerminationSignal*>(termination_signal_addr);
    volatile auto edm_local_sync_ptr = reinterpret_cast<volatile tt_l1_ptr uint32_t*>(edm_local_sync_ptr_addr);
    volatile auto edm_status_ptr = reinterpret_cast<volatile tt_l1_ptr tt::tt_fabric::EDMStatus*>(edm_status_ptr_addr);

    // In persistent mode, we must rely on static addresses for our local semaphores that are locally
    // initialized, rather than metal device APIs. This way different subdevice programs can reliably
    // resolve the semaphore addresses on the EDM core

    size_t arg_idx = 0U;
    ///////////////////////
    // Common runtime args:
    ///////////////////////
    uint32_t local_sender_channel_0_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_1_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_2_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_3_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_4_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_5_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_6_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_7_connection_semaphore_addr = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_0_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_1_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_2_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_3_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_4_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_5_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_6_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
    uint32_t local_sender_channel_7_connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);

    // downstream EDM VC0 connection info
    const auto has_downstream_edm_vc0_buffer_connection = get_arg_val<uint32_t>(arg_idx++);

    // For 2D: read 3 buffer base addresses, NOC coords, and registration addresses (one per compact index)
    // For 1D: reads as 1D and only uses first element
#if defined(FABRIC_2D)
    const auto downstream_edm_vc0_buffer_base_addresses_index = arg_idx;
    arg_idx += NUM_DOWNSTREAM_SENDERS_VC0;
#else
    const auto downstream_edm_vc0_buffer_base_address = get_arg_val<uint32_t>(arg_idx++);
#endif

    const auto downstream_edm_vc0_noc_x = get_arg_val<uint32_t>(arg_idx++);
    const auto downstream_edm_vc0_noc_y = get_arg_val<uint32_t>(arg_idx++);

#if defined(FABRIC_2D)
    // initalize downstream_edm_vc0_worker array of arrays
    //
    {
        using worker_location_info_base_type =
            std::integral_constant<size_t, NUM_DOWNSTREAM_SENDERS_VC0>;
        using worker_index_semaphore_base_type =
            std::integral_constant<size_t, (2U * NUM_DOWNSTREAM_SENDERS_VC0)>; // (NUM_DOWNSTREAM_SENDERS_VC0 << 1)
        using total_loop_values_type =
            std::integral_constant<size_t, (3U * NUM_DOWNSTREAM_SENDERS_VC0)>; // (NUM_DOWNSTREAM_SENDERS_VC0 << 1) + NUM_DOWNSTREAM_SENDERS_VC0
        
        // TODO unroll using template meta programming structure
        {
            // Manually unroll loop for RV32I optimization - typically 3 channels for 2D fabric
            {
                constexpr const size_t i = 0U;
                auto & downstream_edm_vc0_worker_i = downstream_edm_vc0_worker[i];
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_REGISTRATION_ID] =
                    get_arg_val<uint32_t>(arg_idx+i); // registration id
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_LOCATION_INFO_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_location_info_base_type::value); // location info address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_INDEX_SEMAPHORE_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_index_semaphore_base_type::value); // buffer index semaphore address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_BUFFER_BASE_ADDRESSES] =
                    get_arg_val<uint32_t>(i+downstream_edm_vc0_buffer_base_addresses_index); // buffer base address
            }
            
            if constexpr (NUM_DOWNSTREAM_SENDERS_VC0 > 1U) {
                constexpr const size_t i = 1U;
                auto & downstream_edm_vc0_worker_i = downstream_edm_vc0_worker[i];
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_REGISTRATION_ID] =
                    get_arg_val<uint32_t>(arg_idx+i); // registration id
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_LOCATION_INFO_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_location_info_base_type::value); // location info address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_INDEX_SEMAPHORE_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_index_semaphore_base_type::value); // buffer index semaphore address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_BUFFER_BASE_ADDRESSES] =
                    get_arg_val<uint32_t>(i+downstream_edm_vc0_buffer_base_addresses_index); // buffer base address
            }
            
            if constexpr (NUM_DOWNSTREAM_SENDERS_VC0 > 2U) {
                constexpr const size_t i = 2U;
                auto & downstream_edm_vc0_worker_i = downstream_edm_vc0_worker[i];
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_REGISTRATION_ID] =
                    get_arg_val<uint32_t>(arg_idx+i); // registration id
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_LOCATION_INFO_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_location_info_base_type::value); // location info address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_WORKER_INDEX_SEMAPHORE_ADDRESSES] =
                    get_arg_val<uint32_t>((arg_idx+i)+worker_index_semaphore_base_type::value); // buffer index semaphore address
                downstream_edm_vc0_worker_i[DOWNSTREAM_EDM_VC0_BUFFER_BASE_ADDRESSES] =
                    get_arg_val<uint32_t>(i+downstream_edm_vc0_buffer_base_addresses_index); // buffer base address
            }
        }
        
        arg_idx += total_loop_values_type::value;
    }
#else
    const auto downstream_edm_vc0_worker_registration_id = get_arg_val<uint32_t>(arg_idx++);
    const auto downstream_edm_vc0_worker_location_info_address = get_arg_val<uint32_t>(arg_idx++);
    const auto downstream_edm_vc0_buffer_index_semaphore_address = get_arg_val<uint32_t>(arg_idx++);
#endif

    // unused - to be deleted
    [[maybe_unused]]
    const auto downstream_vc0_noc_interface_buffer_index_local_addr = 0U;

    // Read MAX_NUM_SENDER_CHANNELS teardown semaphores (host packs builder_config::num_max_sender_channels = 8)
    /*
        these scalar values are untouched until loaded into array - this removes the
        need for these variables as intermediaries by loading them into the array directly.
    */

    alignas(sizeof(uint32_t)) auto const& local_sem_for_teardown_from_downstream_edm =
        take_first_n_elements<NUM_DOWNSTREAM_CHANNELS, MAX_NUM_SENDER_CHANNELS, uint32_t>(
            std::array<uint32_t, MAX_NUM_SENDER_CHANNELS>{
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_0
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_1
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_2
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_3
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_4
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_5
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_6
                get_arg_val<uint32_t>(arg_idx++),   // my_sem_for_teardown_from_edm_7
            });

    ////////////////////////
    // Sender runtime args
    ////////////////////////
    // Read MAX_NUM_SENDER_CHANNELS sender worker semaphore pointers (host packs builder_config::num_max_sender_channels
    // = 8)
    auto sender0_worker_semaphore_ptr = reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+1U));
    
    const size_t local_sender_channel_0_connection_buffer_index_addr =
        local_sender_channel_0_connection_buffer_index_id;

    //  initialize the statically allocated "semaphores"
    //  loading the address into a volatile pointer and using the pointer
    //  to set the value to 0 peroforms significantly faster on erisc harts
    //  this particular pattern is on display in other parts of this file;
    //  leave alone unless otherwise necessary.
    //
    // TODO wrap using template meta programming structure
    {
        if constexpr (is_sender_channel_serviced[0U]) {
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_0_connection_semaphore_addr) = 0U;
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_0_connection_buffer_index_addr) = 0U;
            *reinterpret_cast<volatile uint32_t*>(sender0_worker_semaphore_ptr) = 0U; // note for the other accesses -> (arg_idx+1U)
        }
        if constexpr (is_sender_channel_serviced[1U]) {
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_1_connection_semaphore_addr) = 0U;
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_1_connection_buffer_index_id) = 0U;
            auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+2U));
            *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
        }
        if constexpr (is_sender_channel_serviced[2U]) {
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_2_connection_semaphore_addr) = 0U;
            *reinterpret_cast<volatile uint32_t*>(local_sender_channel_2_connection_buffer_index_id) = 0U;
            auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+3U));
            *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
        }
        if constexpr (is_2d_fabric) {
            if constexpr (is_sender_channel_serviced[3U]) {
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_3_connection_semaphore_addr) = 0U;
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_3_connection_buffer_index_id) = 0U;
                auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+4U));
                *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
            }
            if constexpr (is_sender_channel_serviced[4U]) {
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_4_connection_semaphore_addr) = 0U;
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_4_connection_buffer_index_id) = 0U;
                auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+5U));
                *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
            }
            if constexpr (is_sender_channel_serviced[5U]) {
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_5_connection_semaphore_addr) = 0U;
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_5_connection_buffer_index_id) = 0U;
                auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+6U));
                *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
            }
            if constexpr (is_sender_channel_serviced[6U]) {
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_6_connection_semaphore_addr) = 0U;
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_6_connection_buffer_index_id) = 0U;
                auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+7U));
                *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
            }
            if constexpr (is_sender_channel_serviced[7U]) {
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_7_connection_semaphore_addr) = 0U;
                *reinterpret_cast<volatile uint32_t*>(local_sender_channel_7_connection_buffer_index_id) = 0U;
                auto sem = *reinterpret_cast<volatile uint32_t*>(get_arg_val<uint32_t>(arg_idx+8U));
                *reinterpret_cast<volatile uint32_t*>(sem) = 0U;
            }
        }
    }

    // skip the 8 previous arguments previously
    // these pointers were stored into the following
    // local variables: sender(0-7)_worker_semaphore_ptr
    //
    arg_idx += 8U;

    ///////////////////////////////////////////////
    // Local tensix (relay) connection runtime args
    // UDM mode only - packed at end of runtime args
    ///////////////////////////////////////////////
    const auto has_local_tensix_relay_connection = get_arg_val<uint32_t>(arg_idx++);

    // SFINAE-friendly type selection; `if constexpr` removes the deadcode
    // when `udm_mode` is false but the type must still be valid when the
    // constexpr and templated type is instantiated. using the inverted value
    // creates a *valid*, but ultimately, *unused* type in the non-UDM mode case (disabled).
    //
    using udm_mode_type =
        std::integral_constant<bool, !udm_mode>;

    using local_tensix_relay_type =
        local_tensix_relay_cond_t< udm_mode_type::value >;

    [[maybe_unused]]
    local_tensix_relay_type local_tensix_relay{};

    if constexpr (udm_mode) {
        if (has_local_tensix_relay_connection) {
            local_tensix_relay.buffer_base_address = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.noc_x = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.noc_y = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.worker_registration_id = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.worker_location_info_address = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.free_slots_stream_id = get_arg_val<uint32_t>(arg_idx++);
            local_tensix_relay.connection_buffer_index_id = get_arg_val<uint32_t>(arg_idx++);
        }
    }

    *edm_status_ptr = tt::tt_fabric::EDMStatus::STARTED;

    //////////////////////////////
    //////////////////////////////
    //        Object Setup
    //////////////////////////////
    //////////////////////////////

    // Hack for mux mode until all remaining VC1 logic is removed from fabric
    // Needed so `downstream_edm_noc_interfaces_vc0` can be initialized properly below
    // Issue #33360 TODO: Create a new array for downstream receiver stream IDs
    // so we can remove this hack.
    alignas(sizeof(uint32_t)) std::array<uint32_t, NUM_SENDER_CHANNELS> local_sender_channel_free_slots_stream_ids;

    // std::array<uint32_t, NUM_SENDER_CHANNELS == 1 ? 2 : NUM_SENDER_CHANNELS>
    // local_sender_channel_free_slots_stream_ids;

    // create the remote receiver channel buffers using multi-pool system
    auto remote_receiver_channels = tt::tt_fabric::MultiPoolEthChannelBuffers<
        PACKET_HEADER_TYPE,
        eth_remote_channel_pools_args,
        REMOTE_RECEIVER_TO_POOL_TYPE,
        REMOTE_RECEIVER_TO_POOL_IDX>::make();

    auto local_receiver_channels =
        tt::tt_fabric::MultiPoolEthChannelBuffers<
            PACKET_HEADER_TYPE,
            channel_pools_args,
            RECEIVER_TO_POOL_TYPE,
            RECEIVER_TO_POOL_IDX
        >::make();

    auto local_sender_channels = tt::tt_fabric::MultiPoolSenderEthChannelBuffers<
        PACKET_HEADER_TYPE,
        channel_pools_args,
        SENDER_TO_POOL_TYPE,
        SENDER_TO_POOL_IDX>::make();

    alignas(sizeof(size_t)) std::array<size_t, NUM_SENDER_CHANNELS> local_sender_connection_live_semaphore_addresses =
        take_first_n_elements<NUM_SENDER_CHANNELS, MAX_NUM_SENDER_CHANNELS, size_t>(
            std::array<size_t, MAX_NUM_SENDER_CHANNELS>{
                local_sender_channel_0_connection_semaphore_addr,
                local_sender_channel_1_connection_semaphore_addr,
                local_sender_channel_2_connection_semaphore_addr,
                local_sender_channel_3_connection_semaphore_addr,
                local_sender_channel_4_connection_semaphore_addr,
                local_sender_channel_5_connection_semaphore_addr,
                local_sender_channel_6_connection_semaphore_addr,
                local_sender_channel_7_connection_semaphore_addr});

    alignas(sizeof(size_t)) std::array<size_t, NUM_SENDER_CHANNELS> local_sender_connection_info_addresses =
        take_first_n_elements<NUM_SENDER_CHANNELS, MAX_NUM_SENDER_CHANNELS, size_t>(
            std::array<size_t, MAX_NUM_SENDER_CHANNELS>{
                local_sender_channel_0_connection_info_addr,
                local_sender_channel_1_connection_info_addr,
                local_sender_channel_2_connection_info_addr,
                local_sender_channel_3_connection_info_addr,
                local_sender_channel_4_connection_info_addr,
                local_sender_channel_5_connection_info_addr,
                local_sender_channel_6_connection_info_addr,
                local_sender_channel_7_connection_info_addr});
    
    // TODO implement using a recursive template meta-program structure
    {
        // Manually unroll loop for RV32I optimization - typically 2-8 channels
        {
            constexpr const size_t i = 0U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 1U) {
            constexpr const size_t i = 1U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 2U) {
            constexpr const size_t i = 2U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 3U) {
            constexpr const size_t i = 3U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 4U) {
            constexpr const size_t i = 4U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 5U) {
            constexpr const size_t i = 5U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 6U) {
            constexpr const size_t i = 6U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
        
        if constexpr (NUM_SENDER_CHANNELS > 7U) {
            constexpr const size_t i = 7U;
            auto connection_worker_info_ptr = reinterpret_cast<volatile tt::tt_fabric::EDMChannelWorkerLocationInfo*>(
                local_sender_connection_info_addresses[i]);
            connection_worker_info_ptr->edm_read_counter = 0U;
        }
    }
    
    // create the sender channel worker interfaces with input array of number of buffers
    alignas(sizeof(uint32_t)) auto local_sender_channel_worker_interfaces =
        tt::tt_fabric::EdmChannelWorkerInterfaces<tt::tt_fabric::worker_handshake_noc, SENDER_NUM_BUFFERS_ARRAY>::make(
            std::make_index_sequence<NUM_SENDER_CHANNELS>{});

    // TODO: change to TMP.
    alignas(sizeof(uint32_t)) std::array<RouterToRouterSender<DOWNSTREAM_SENDER_NUM_BUFFERS_VC0>, NUM_DOWNSTREAM_SENDERS_VC0>
        downstream_edm_noc_interfaces_vc0;
    populate_local_sender_channel_free_slots_stream_id_ordered_map(
        local_sender_channel_free_slots_stream_ids);

    if (has_downstream_edm_vc0_buffer_connection) {
        // Only bit 0 is set for 1D
        // For 2D: 3 bits set for compact indices 0, 1, 2 (excluding router's own direction)
        uint32_t has_downstream_edm = has_downstream_edm_vc0_buffer_connection & 0x7U;  // 3-bit mask
        uint32_t compact_index = 0U;
        uint32_t shift_val = 0U;
        while (has_downstream_edm) {
            if (has_downstream_edm & 0x1U) {
#if defined(FABRIC_2D)
                auto & downstream_edm_vc0_worker_ci = downstream_edm_vc0_worker[compact_index];
#endif
                auto teardown_sem_address = local_sem_for_teardown_from_downstream_edm[compact_index];
                // reset the handshake addresses to 0 (this is for router -> router handshake for connections over noc)
                *reinterpret_cast<volatile uint32_t*>(teardown_sem_address) = 0U;

#if defined(FABRIC_2D)
#define VC_0_FEE_SLOT_STREAM_INDEX (compact_index)
#else
#define VC_0_FEE_SLOT_STREAM_INDEX (0U)
#endif
                auto const receiver_channel_free_slots_stream_id =
                    StreamId{vc_0_free_slots_stream_ids[VC_0_FEE_SLOT_STREAM_INDEX]};

                // (x << 3) == (x * 8)
                //
                shift_val = (compact_index << 3U) & 0xFFU;
                new (&downstream_edm_noc_interfaces_vc0[compact_index])
                    RouterToRouterSender<DOWNSTREAM_SENDER_NUM_BUFFERS_VC0>(
                        // persistent_mode -> hardcode to false for 1D because for 1D, EDM -> EDM
                        // connections we must always use semaphore lookup
                        // For 2D, downstream_edm_vc0_semaphore_id is an address.
                        is_persistent_fabric,
                        (downstream_edm_vc0_noc_x >> shift_val),
                        (downstream_edm_vc0_noc_y >> shift_val),
#if defined(FABRIC_2D)
//                        downstream_edm_vc0_buffer_base_addresses[compact_index],
                        downstream_edm_vc0_worker_ci[DOWNSTREAM_EDM_VC0_BUFFER_BASE_ADDRESSES],
#else
                        downstream_edm_vc0_buffer_base_address,
#endif
                        DOWNSTREAM_SENDER_NUM_BUFFERS_VC0,
#if defined(FABRIC_2D)
                        // connection handshake address on downstream edm
                        downstream_edm_vc0_worker_ci[DOWNSTREAM_EDM_VC0_WORKER_REGISTRATION_ID],
                        // worker location info address on downstream edm
                        // written by this interface when it connects to the downstream edm
                        // so that the downstream edm knows who its upstream peer is
                        downstream_edm_vc0_worker_ci[DOWNSTREAM_EDM_VC0_WORKER_LOCATION_INFO_ADDRESSES],
#else
                        downstream_edm_vc0_worker_registration_id,
                        downstream_edm_vc0_worker_location_info_address,
#endif
                        channel_buffer_size,
                // Used to park current write pointer value at the downstream edm
                // when this interface disconnects from the downstream edm.
#if defined(FABRIC_2D)
                        downstream_edm_vc0_worker_ci[DOWNSTREAM_EDM_VC0_WORKER_INDEX_SEMAPHORE_ADDRESSES],
#else
                        downstream_edm_vc0_buffer_index_semaphore_address,
#endif
                        0,  // Unused for Router->Router connections. Router->Router always uses stream registers for
                            // credits. Used by Worker->Router connections. This is an address in the worker's L1. The
                            // Router that a Worker adapter is connected to writes its read counter to this address. The
                            // worker uses this to calculate free slots in the router's sender channel.
                        reinterpret_cast<volatile uint32_t* const>(teardown_sem_address),
                        downstream_vc0_noc_interface_buffer_index_local_addr,  // keep common, since its a scratch noc
                                                                               // read dest.

#if defined(FABRIC_2D)
                        get_vc0_downstream_sender_channel_free_slots_stream_id(compact_index),
#else
                        // Issue #33360 TODO: Create a new array for explicitly holding downstream receiver stream IDs
                        // so we can remove this hack.
                        sender_channel_1_free_slots_stream_id,
#endif
                        // This is our local stream register for the copy of the downstream router's
                        // free slots
                        receiver_channel_free_slots_stream_id,
                        receiver_channel_forwarding_data_cmd_buf_ids[0U],
                        receiver_channel_forwarding_sync_cmd_buf_ids[0U]);
                // Only receiver channel servicing cores should be setting up the noc cmd buf.
                if constexpr (NUM_ACTIVE_ERISCS == 1 && !FORCE_ALL_PATHS_TO_USE_SAME_NOC) {
                    downstream_edm_noc_interfaces_vc0[compact_index]
                        .template setup_edm_noc_cmd_buf<
                            tt::tt_fabric::edm_to_downstream_noc,
                            tt::tt_fabric::forward_and_local_write_noc_vc>();
                }
            }
            compact_index++;
            has_downstream_edm >>= 1;
        }
    }

    // Setup local tensix relay connection (UDM mode only)
    // This is a separate connection path from downstream EDM connections
    // Relay handles forwarding packets to local chip workers
    // Uses dedicated stream IDs and L1 locations to avoid assumptions about direction indexing
    // LOCAL_RELAY_NUM_BUFFERS comes from compile-time args (propagated from relay config)

    using RouterRouterToRouterSenderRelayType =
        RouterToRouterSender_t< udm_mode_type::value >;

    [[maybe_unused]]
    RouterRouterToRouterSenderRelayType local_relay_interface;

    if constexpr (udm_mode) {
        if (has_local_tensix_relay_connection) {
            // Reuse RouterToRouterSender for relay connection
            // Relay is just another sender interface, but pointing to local tensix instead of remote router

            new (&local_relay_interface) RouterRouterToRouterSenderRelayType(
                true,  // persistent_mode - relay is always a persistent connection
                local_tensix_relay.noc_x,
                local_tensix_relay.noc_y,
                local_tensix_relay.buffer_base_address,
                LOCAL_RELAY_NUM_BUFFERS,  // Use compile-time constant
                local_tensix_relay.worker_registration_id,
                local_tensix_relay.worker_location_info_address,
                channel_buffer_size,
                local_tensix_relay.connection_buffer_index_id,  // From runtime args - dedicated L1 location for relay
                                                                // connection
                0,        // worker read counter address - unused for Router->Relay (uses stream registers)
                nullptr,  // teardown semaphore - router never calls close on relay
                0,        // buffer_index_local_addr - scratch space for noc reads
                // Remote stream: relay's free slots stream (what relay publishes) - from runtime args
                StreamId{local_tensix_relay.free_slots_stream_id},
                // Local stream: our copy of relay's free slots - dedicated stream ID for relay
                StreamId{tensix_relay_local_free_slots_stream_id},
                receiver_channel_forwarding_data_cmd_buf_ids[0U],
                receiver_channel_forwarding_sync_cmd_buf_ids[0U]);

            // Setup NOC command buffer for relay interface
            if constexpr (NUM_ACTIVE_ERISCS == 1 && !FORCE_ALL_PATHS_TO_USE_SAME_NOC) {
                local_relay_interface.template setup_edm_noc_cmd_buf<
                    tt::tt_fabric::edm_to_downstream_noc,
                    tt::tt_fabric::forward_and_local_write_noc_vc>();
            }
        }
    }

    // helps ubenchmark performance
    __asm__("nop");

    // initialize the local receiver channel buffers
    local_receiver_channels.init<channel_pools_args>(
        channel_buffer_size,
        sizeof(PACKET_HEADER_TYPE));

    // initialize the remote receiver channel buffers
    remote_receiver_channels.init<eth_remote_channel_pools_args>(
        channel_buffer_size,
        sizeof(PACKET_HEADER_TYPE));

    // initialize the local sender channel worker interfaces
    local_sender_channels.init<channel_pools_args>(
        channel_buffer_size,
        sizeof(PACKET_HEADER_TYPE));

    // initialize the local sender channel worker interfaces
    // Sender channel 0 is always for local worker in the new design
    constexpr auto sender_channel = 0U;
    if constexpr (is_sender_channel_serviced[sender_channel]) {
        init_local_sender_channel_worker_interfaces(
            local_sender_connection_live_semaphore_addresses,
            local_sender_connection_info_addresses,
            local_sender_channel_worker_interfaces);
    }

    __asm__("nop");

    WriteTransactionIdTracker<
        RECEIVER_NUM_BUFFERS_ARRAY[0U],
        NUM_TRANSACTION_IDS,
        0U,
        edm_to_local_chip_noc,
        edm_to_downstream_noc>
        receiver_channel_0_trid_tracker;
    receiver_channel_0_trid_tracker.init();

    constexpr bool const use_posted_writes_for_connection_open =
#ifdef ARCH_BLACKHOLE
    // A Blackhole hardware bug requires all noc inline writes to be non-posted so we hardcode to false here
    // A more detailed description can be found in `noc_inline_dw_write` in the `dataflow_api` header file
    false
#else
    true
#endif
    ;

    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        // This barrier is here just in case the initialization process of any of the sender/receiver channel
        // implementations require any assumptions about channel contents or anything similar. Without it there
        // is possibility of a race. The race would be where the the risc core responsible for Ethernet level handshake
        // completes before the other risc finishes setup of channel/credit datastructures. If that happened, then
        // it would be possible for the other (remote) Ethernet core to start sending packets/credits to our core before
        // all of our cores are done setup, leading to potentially undefined behavior.
        //
        // Whether or not there truly is a race in a given snapshot/commit hash is not relevant. The intention with this
        // is to avoid all possible footguns as implementations of underlying datastructures potenntially change over
        // time.
        wait_for_other_local_erisc();
    }
    if constexpr (enable_ethernet_handshake) {
        if constexpr (is_handshake_sender) {
            erisc::datamover::handshake::sender_side_handshake(
                handshake_addr, DEFAULT_HANDSHAKE_CONTEXT_SWITCH_TIMEOUT);
        } else {
            erisc::datamover::handshake::receiver_side_handshake(
                handshake_addr, DEFAULT_HANDSHAKE_CONTEXT_SWITCH_TIMEOUT);
        }

        *edm_status_ptr = tt::tt_fabric::EDMStatus::REMOTE_HANDSHAKE_COMPLETE;

        if constexpr (wait_for_host_signal) {
            if constexpr (is_local_handshake_master) {
                wait_for_notification<ENABLE_RISC_CPU_DATA_CACHE>((uint32_t)edm_local_sync_ptr, num_local_edms - 1U);
                // This master sends notification to self for multi risc in single eth core case,
                // This still send to self even though with single risc core case, but no side effects
                constexpr uint32_t const exclude_eth_chan = std::numeric_limits<uint32_t>::max();
                notify_subordinate_routers(
                    edm_channels_mask, exclude_eth_chan, (uint32_t)edm_local_sync_ptr, num_local_edms);
            } else {
                notify_master_router(local_handshake_master_eth_chan, (uint32_t)edm_local_sync_ptr);
                wait_for_notification<ENABLE_RISC_CPU_DATA_CACHE>((uint32_t)edm_local_sync_ptr, num_local_edms);
            }

            *edm_status_ptr = tt::tt_fabric::EDMStatus::LOCAL_HANDSHAKE_COMPLETE;

            // 1. All risc cores wait for READY_FOR_TRAFFIC signal
            // 2. All risc cores in master eth core receive signal from host and exits from this wait
            //    Other subordinate risc cores wait for this signal
            // 4. The other subordinate risc cores receive the READY_FOR_TRAFFIC signal and exit from this wait
            {
                union {
                    volatile tt::tt_fabric::EDMStatus* volatile ptr;
                    uint32_t addr;
                } edm_status_ptr_cast;
                edm_status_ptr_cast.ptr = edm_status_ptr;
                wait_for_notification<ENABLE_RISC_CPU_DATA_CACHE>(edm_status_ptr_cast.addr, tt::tt_fabric::EDMStatus::READY_FOR_TRAFFIC);
            }
            
            if constexpr (is_local_handshake_master) {
                // 3. Only master risc core notifies all subordinate risc cores (except subordinate riscs in master eth
                // core)
                notify_subordinate_routers(
                    edm_channels_mask,
                    local_handshake_master_eth_chan,
                    (uint32_t)edm_status_ptr,
                    tt::tt_fabric::EDMStatus::READY_FOR_TRAFFIC);
            }
        }
    }

    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        wait_for_other_local_erisc();
    }

    // if enable the tensix extension, then before open downstream connection, need to wait for downstream tensix ready
    // for connection.
    if constexpr (num_ds_or_local_tensix_connections) {
        wait_for_notification<ENABLE_RISC_CPU_DATA_CACHE>((uint32_t)edm_local_tensix_sync_ptr_addr, num_ds_or_local_tensix_connections);
    }

    if constexpr (is_2d_fabric) {
        uint32_t has_downstream_edm = has_downstream_edm_vc0_buffer_connection & 0x7U;  // 3-bit mask
        uint32_t edm_index = 0U;
        if constexpr (is_receiver_channel_serviced[0U]) {
            while (has_downstream_edm) {
                if (has_downstream_edm & 0x1U) {
                    // open connections with available downstream edms
                    downstream_edm_noc_interfaces_vc0[edm_index]
                        .template open<
                            false,
                            use_posted_writes_for_connection_open,
                            tt::tt_fabric::worker_handshake_noc>();
                }
                edm_index++;
                has_downstream_edm >>= 1U;
            }
        }
        if constexpr (udm_mode) {
            if (has_local_tensix_relay_connection) {
                // open connection here to relay kernel
                local_relay_interface
                    .template open<false, use_posted_writes_for_connection_open, tt::tt_fabric::worker_handshake_noc>();
            }
        }
    } else {
        // We can check just the first index because all receiver channels are serviced by the same core
        if constexpr (is_receiver_channel_serviced[0U]) {
            if (has_downstream_edm_vc0_buffer_connection) {
                downstream_edm_noc_interfaces_vc0[0U]
                    .template open<false, use_posted_writes_for_connection_open, tt::tt_fabric::worker_handshake_noc>();
                ASSERT(
                    get_ptr_val(downstream_edm_noc_interfaces_vc0[0U].get_worker_credits_stream_id()) ==
                    DOWNSTREAM_SENDER_NUM_BUFFERS_VC0);
            }
        }
    }

    if constexpr (NUM_ACTIVE_ERISCS > 1) {
        wait_for_other_local_erisc();
    }

    if constexpr (is_receiver_channel_serviced[0U] and NUM_ACTIVE_ERISCS > 1U) {
        // Two erisc mode requires us to reorder the cmd buf programming/state setting
        // because we need to reshuffle some of our cmd_buf/noc assignments around for
        // just the fabric bringup phase. These calls are also located earlier for the
        // single erisc mode
        if constexpr (!FORCE_ALL_PATHS_TO_USE_SAME_NOC) {
            uint32_t has_downstream_edm = has_downstream_edm_vc0_buffer_connection & 0x7U;  // 3-bit mask
            uint32_t edm_index = 0U;            
            while (has_downstream_edm) {
                if (has_downstream_edm & 0x1U) {
                    downstream_edm_noc_interfaces_vc0[edm_index]
                        .template setup_edm_noc_cmd_buf<
                            tt::tt_fabric::edm_to_downstream_noc,
                            tt::tt_fabric::forward_and_local_write_noc_vc>();
                }
                edm_index++;
                has_downstream_edm >>= 1U;
            }
        }
    }

    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        wait_for_other_local_erisc();
    }
    WAYPOINT("FSCW");
    wait_for_static_connection_to_ready(
        local_sender_channel_worker_interfaces, local_sender_channel_free_slots_stream_ids);
    WAYPOINT("FSCD");

    if constexpr (NUM_ACTIVE_ERISCS > 1U) {
        wait_for_other_local_erisc();
    }

    //////////////////////////////
    //////////////////////////////
    //        MAIN LOOP
    //////////////////////////////
    //////////////////////////////
    run_fabric_edm_main_loop<
        NUM_RECEIVER_CHANNELS,
        RouterToRouterSender<DOWNSTREAM_SENDER_NUM_BUFFERS_VC0>,
        RouterToRouterSender<LOCAL_RELAY_NUM_BUFFERS>>(
        local_receiver_channels,
        local_sender_channels,
        local_sender_channel_worker_interfaces,
        downstream_edm_noc_interfaces_vc0,
        // pass in the relay adpator
        local_relay_interface,
        remote_receiver_channels,
        termination_signal_ptr,
        receiver_channel_0_trid_tracker,
        local_sender_channel_free_slots_stream_ids);
    WAYPOINT("LPDN");

    // we force these values to a non-zero value so that if we run the fabric back to back,
    // and we can reliably probe from host that this kernel has initialized properly.
    // Sender channel 0 is always for local worker in both 1D and 2D
    *reinterpret_cast<volatile uint32_t*>(local_sender_channel_0_connection_semaphore_addr) = 99U;
    *reinterpret_cast<volatile uint32_t*>(local_sender_channel_0_connection_buffer_index_addr) = 99U;
    *reinterpret_cast<volatile uint32_t*>(sender0_worker_semaphore_ptr) = 99U;

    // make sure all the noc transactions are acked before re-init the noc counters
    teardown(termination_signal_ptr, edm_status_ptr, receiver_channel_0_trid_tracker);

    set_l1_data_cache<false>();
    WAYPOINT("DONE");
}
