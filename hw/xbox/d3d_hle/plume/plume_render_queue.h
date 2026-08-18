#ifndef XGPU_PLUME_RENDER_QUEUE_H
#define XGPU_PLUME_RENDER_QUEUE_H

/* Bounded render-job queue / slot / completion-ticket state machine for the
 * async present pipeline (docs/technical/plume-render-worker-ownership.md,
 * sections 3-4). Pure logic: no threads, no RHI, no time. The eventual
 * render worker wraps exactly one instance under one mutex; these
 * transitions ARE the design's state machine and are tested natively
 * before any thread exists (rollout stage 1).
 *
 * Slot lifecycle:  Free -> Reserved -> Queued -> Executing -> InFlight
 *                  -> (reclaim) -> Free
 * Ticket lifecycle: Pending -> Done | Failed, signalled exactly once.
 * A wait slot can never be handed out again before its prior job's
 * reclaim; at most one Present ticket may be Pending at a time. */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace xgpu {
namespace plume {

enum class RenderSlotState : uint8_t {
    Free, Reserved, Queued, Executing, InFlight
};
enum class RenderTicketState : uint8_t { Pending, Done, Failed };
enum class RenderJobKind : uint8_t { WaitBatch, Present };

class RenderQueue {
public:
    static constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

    explicit RenderQueue(uint32_t waitSlots)
        : m_slots(waitSlots + 1u) {}

    uint32_t waitSlotCount() const {
        return static_cast<uint32_t>(m_slots.size()) - 1u;
    }
    uint32_t presentSlot() const { return waitSlotCount(); }
    bool healthy() const { return m_healthy; }
    size_t queueDepth() const { return m_fifo.size(); }

    /* Guest side: reserve a wait slot. kInvalidSlot when none is Free or
     * the queue is unhealthy; blockingTicket() then names the oldest
     * Pending ticket the caller must wait before retrying. */
    uint32_t reserveWaitSlot() {
        if (!m_healthy)
            return kInvalidSlot;
        for (uint32_t i = 0; i < waitSlotCount(); ++i) {
            if (m_slots[i].state == RenderSlotState::Free) {
                m_slots[i].state = RenderSlotState::Reserved;
                return i;
            }
        }
        return kInvalidSlot;
    }

    /* Guest side: at most one unfinished Present may be outstanding. False
     * while a Present ticket is Pending (caller waits that ticket first),
     * while the present slot is busy, or when unhealthy. */
    bool reservePresent() {
        if (!m_healthy ||
            m_slots[presentSlot()].state != RenderSlotState::Free)
            return false;
        for (size_t i = m_tickets.size(); i > 0; --i) {
            const Ticket &t = m_tickets[i - 1u];
            if (t.kind == RenderJobKind::Present &&
                t.state == RenderTicketState::Pending)
                return false;
        }
        m_slots[presentSlot()].state = RenderSlotState::Reserved;
        return true;
    }

    /* Guest side: queue a job on a slot reserved above. Returns the ticket
     * sequence, or 0 on misuse (wrong slot kind, slot not Reserved) or
     * when unhealthy. Sequences start at 1 and are the strict FIFO order. */
    uint64_t enqueue(RenderJobKind kind, uint32_t slot) {
        if (!m_healthy || slot >= m_slots.size() ||
            m_slots[slot].state != RenderSlotState::Reserved)
            return 0;
        if ((kind == RenderJobKind::Present) != (slot == presentSlot()))
            return 0;
        m_tickets.push_back(Ticket{kind, RenderTicketState::Pending, slot});
        const uint64_t sequence = m_tickets.size();
        m_slots[slot].state = RenderSlotState::Queued;
        m_slots[slot].sequence = sequence;
        m_fifo.push_back(sequence);
        return sequence;
    }

    /* Oldest Pending ticket, or 0 when none. What a refused reservation
     * waits on. */
    uint64_t blockingTicket() const {
        for (size_t i = 0; i < m_tickets.size(); ++i) {
            if (m_tickets[i].state == RenderTicketState::Pending)
                return i + 1u;
        }
        return 0;
    }

    /* Owner side: take the next job strictly in enqueue order. */
    bool beginNext(uint64_t *sequence, RenderJobKind *kind, uint32_t *slot) {
        if (m_fifo.empty())
            return false;
        const uint64_t next = m_fifo.front();
        m_fifo.pop_front();
        const Ticket &t = m_tickets[next - 1u];
        m_slots[t.slot].state = RenderSlotState::Executing;
        if (sequence)
            *sequence = next;
        if (kind)
            *kind = t.kind;
        if (slot)
            *slot = t.slot;
        return true;
    }

    /* Owner side: the job's command list was submitted; its fence is armed. */
    void markSubmitted(uint64_t sequence) {
        Ticket *t = ticketAt(sequence);
        if (t && t->state == RenderTicketState::Pending)
            m_slots[t->slot].state = RenderSlotState::InFlight;
    }

    /* Owner side: fence waited and every queued download for this job is
     * packed into guest RAM. Frees the slot and signals the ticket Done.
     * packedGenerations lists the surface generations whose guest bytes
     * are now current (consumed by targeted CPU-read waits). */
    void completeReclaim(uint64_t sequence,
                         const uint64_t *packedGenerations = nullptr,
                         size_t packedCount = 0) {
        Ticket *t = ticketAt(sequence);
        if (!t || t->state != RenderTicketState::Pending)
            return;
        t->packed.assign(packedGenerations,
                         packedGenerations + packedCount);
        t->state = RenderTicketState::Done;
        m_slots[t->slot].state = RenderSlotState::Free;
        m_slots[t->slot].sequence = 0;
    }

    /* Owner side: replay/submit/fence failure. Sticky: this ticket and
     * every still-Pending ticket fail, queued jobs never begin, and all
     * further reservations are refused until the queue is recreated. */
    void failFrom(uint64_t sequence) {
        (void)sequence;
        failEverythingPending();
    }

    /* Drain/teardown: every not-yet-signalled ticket fails exactly once;
     * the queue refuses further work. */
    void stopAll() { failEverythingPending(); }

    RenderTicketState ticketState(uint64_t sequence) const {
        const Ticket *t = ticketAt(sequence);
        return t ? t->state : RenderTicketState::Failed;
    }

    /* Newest Done ticket whose reclaim packed `generation`; 0 when none
     * (the caller falls back to a conservative full drain). */
    uint64_t latestTicketForGeneration(uint64_t generation) const {
        for (size_t i = m_tickets.size(); i > 0; --i) {
            const Ticket &t = m_tickets[i - 1u];
            if (t.state != RenderTicketState::Done)
                continue;
            for (uint64_t g : t.packed) {
                if (g == generation)
                    return i;
            }
        }
        return 0;
    }

private:
    struct Ticket {
        RenderJobKind kind;
        RenderTicketState state;
        uint32_t slot;
        std::vector<uint64_t> packed;
    };
    struct Slot {
        RenderSlotState state = RenderSlotState::Free;
        uint64_t sequence = 0;
    };

    Ticket *ticketAt(uint64_t sequence) {
        return (sequence >= 1u && sequence <= m_tickets.size())
                   ? &m_tickets[sequence - 1u] : nullptr;
    }
    const Ticket *ticketAt(uint64_t sequence) const {
        return (sequence >= 1u && sequence <= m_tickets.size())
                   ? &m_tickets[sequence - 1u] : nullptr;
    }

    void failEverythingPending() {
        m_healthy = false;
        for (Ticket &t : m_tickets) {
            if (t.state == RenderTicketState::Pending)
                t.state = RenderTicketState::Failed;
        }
        for (Slot &s : m_slots) {
            s.state = RenderSlotState::Free;
            s.sequence = 0;
        }
        m_fifo.clear();
    }

    std::vector<Slot> m_slots;      /* [0..waitSlots) + present slot */
    std::vector<Ticket> m_tickets;  /* sequence n lives at index n-1 */
    std::deque<uint64_t> m_fifo;    /* strict submission order */
    bool m_healthy = true;
};

} /* namespace plume */
} /* namespace xgpu */

#endif /* XGPU_PLUME_RENDER_QUEUE_H */
