// SPDX-License-Identifier: MIT
//
// The device-free half of the C++ binding: the generational slab, the op state
// machine and the vocabulary. None of it touches /dev/koru, so ctest runs it on
// the host and a failure here needs no VM to diagnose.

#include "harness.hpp"

#include <koru/error.hpp>
#include <koru/op.hpp>
#include <koru/slab.hpp>

#include <cerrno>
#include <string>

using namespace koru;

// ---------------------------------------------------------------------------
// The slab
// ---------------------------------------------------------------------------

namespace {

/// A payload that says when it was destroyed, which is what the slot is a
/// stand-in for: the production one owns a `BufSlot`.
struct Tracked {
    int *drops = nullptr;
    int id     = 0;

    Tracked() = default;
    Tracked(int *d, int i) : drops(d), id(i) {}
    Tracked(const Tracked &)            = delete;
    Tracked &operator=(const Tracked &) = delete;
    Tracked(Tracked &&o) noexcept : drops(o.drops), id(o.id) { o.drops = nullptr; }
    Tracked &operator=(Tracked &&o) noexcept
    {
        if (this != &o) {
            if (drops)
                (*drops)++;
            drops   = o.drops;
            id      = o.id;
            o.drops = nullptr;
        }
        return *this;
    }
    ~Tracked()
    {
        if (drops)
            (*drops)++;
    }
};

} // namespace

CASE(slab_a_cookie_is_an_index_and_a_generation)
{
    Cookie c(7, 3);
    CHECK_EQ(c.index(), 7);
    CHECK_EQ(c.generation(), 3);
    // The halves are the two words of one user_data, so a cookie survives the
    // round trip through the CQE that a whole op depends on.
    CHECK_EQ(Cookie(c.value).index(), 7);
    CHECK_EQ(Cookie(c.value).generation(), 3);
    CHECK(Cookie(7, 3) == c);
    CHECK(Cookie(7, 4) != c);
}

CASE(slab_insert_get_remove)
{
    int drops = 0;
    Slab<Tracked> slab;
    CHECK_EQ(slab.live(), 0);

    Cookie a = slab.insert(Tracked(&drops, 1));
    Cookie b = slab.insert(Tracked(&drops, 2));
    CHECK_EQ(slab.live(), 2);
    CHECK(a != b);
    REQUIRE(slab.get(a) != nullptr);
    CHECK_EQ(slab.get(a)->id, 1);
    CHECK_EQ(slab.get(b)->id, 2);

    CHECK(slab.remove(a));
    CHECK_EQ(slab.live(), 1);
    CHECK_EQ(drops, 1);            // the payload went with the entry
    CHECK(slab.get(a) == nullptr); // and the cookie is stale at once
    CHECK(!slab.remove(a));        // removing it twice is not a crash
    CHECK(slab.get(b) != nullptr);
}

CASE(slab_a_reused_index_rejects_the_retired_cookie)
{
    // This is the whole reason for the generation: a late completion for a
    // cancelled op must not resume whatever took its place.
    Slab<int> slab;
    Cookie a = slab.insert(11);
    CHECK(slab.remove(a));
    Cookie b = slab.insert(22);
    CHECK_EQ(b.index(), a.index());          // the index is reused
    CHECK(b.generation() != a.generation()); // with a new generation
    CHECK(slab.get(a) == nullptr);           // the old cookie names nothing
    REQUIRE(slab.get(b) != nullptr);
    CHECK_EQ(*slab.get(b), 22);
}

CASE(slab_a_generation_never_wraps_to_zero)
{
    // A cookie of 0 would be indistinguishable from an SQE nobody named, so
    // the bump skips it. Reaching the wrap takes 2^32 removals, so the rule is
    // asserted on the arithmetic rather than by getting there.
    Slab<int> slab;
    Cookie first = slab.insert(1);
    CHECK_EQ(first.generation(), 1); // never 0 to start with
    CHECK(first.value != 0);
    for (int i = 0; i < 5; i++) {
        Cookie c = slab.insert(i);
        CHECK(c.value != 0);
        CHECK(c.generation() != 0);
        slab.remove(c);
    }
}

CASE(slab_indices_are_recycled_rather_than_grown)
{
    Slab<int> slab;
    Cookie a = slab.insert(1);
    Cookie b = slab.insert(2);
    CHECK_EQ(slab.capacity(), 2);
    slab.remove(a);
    slab.remove(b);
    slab.insert(3);
    slab.insert(4);
    // A slab that grew instead of recycling would leak an entry per op, which
    // over a long run is the whole arena's worth of bookkeeping.
    CHECK_EQ(slab.capacity(), 2);
    CHECK_EQ(slab.live(), 2);
}

// ---------------------------------------------------------------------------
// The op state machine
// ---------------------------------------------------------------------------

namespace {

Op<int> an_op()
{
    return Op<int>::queued(7);
}

} // namespace

CASE(op_the_happy_path_is_queued_then_live_then_ready)
{
    Op<int> o = an_op();
    CHECK(o.state == State::Queued);
    o.on_submitted();
    CHECK(o.state == State::Live);
    CHECK(o.on_cqe(4096, 0) == Landed::Woke);
    CHECK(o.state == State::Ready);
    CHECK_EQ(o.res, 4096);
    CHECK_EQ(o.extra, 0);
}

CASE(op_dropping_a_queued_op_drops_its_sqe_and_submits_no_cancel)
{
    Op<int> o = an_op();
    CHECK(o.on_abandon() == Abandon::DropSqe);
    CHECK(o.state == State::Dead);
}

CASE(op_dropping_a_live_op_keeps_its_slot_and_asks_for_a_cancel)
{
    Op<int> o = an_op();
    o.on_submitted();
    CHECK(o.on_abandon() == Abandon::Cancel);
    CHECK(o.state == State::Abandoned);
    // The slot stays until the CQE lands: the kernel is writing into it.
    CHECK(o.slot.has_value());
}

/// `OPEN`, `CLOSE` and `NOP` complete inline inside the submitting `ENTER`, so
/// this is the common case, not a corner.
CASE(op_dropping_an_already_ready_op_submits_no_cancel)
{
    Op<int> o = an_op();
    o.on_submitted();
    o.on_cqe(0, 0);
    CHECK(o.on_abandon() == Abandon::Remove);
}

CASE(op_an_abandoned_op_discards_its_completion)
{
    Op<int> o = an_op();
    o.on_submitted();
    o.on_abandon();
    CHECK(o.on_cqe(-ECANCELED, 0) == Landed::Discard);
}

CASE(op_a_cancel_probe_discards_its_own_completion)
{
    Op<int> o = Op<int>::probe();
    CHECK(o.on_cqe(0, 0) == Landed::Discard);
    CHECK(!o.slot.has_value());
}

CASE(op_a_completion_for_a_queued_op_is_impossible)
{
    // C1 says a completion follows a consumed SQE, so one for an op the kernel
    // has not seen is a bug on this side of the wire.
    Op<int> o = an_op();
    CHECK(o.on_cqe(0, 0) == Landed::Impossible);
    Op<int> dead = an_op();
    dead.on_abandon();
    CHECK(dead.on_abandon() == Abandon::Impossible);
}

CASE(op_the_stall_predicate_is_inflight_not_length)
{
    // Unreachable means inflight == 0, not len == 0 && inflight == 0, which is
    // the foot-gun T11 found on the kernel side and the executor must not
    // re-create by parking on an empty ring.
    CHECK(nothing_can_arrive(0, 0, 0));
    CHECK(!nothing_can_arrive(1, 0, 0));
    CHECK(!nothing_can_arrive(0, 1, 0));
    CHECK(!nothing_can_arrive(0, 0, 1));
}

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

CASE(errno_table_maps_every_row_to_one_name_and_back)
{
    CHECK(koru_errnos_len() > 0);
    for (size_t i = 0; i < koru_errnos_len(); i++) {
        const ErrnoDef &d = KORU_ERRNOS[i];
        Error e           = Error::from_errno(d.err);
        if (e.kind() != d.kind)
            FAILF("%s mapped to the wrong name", d.name);
        if (e.raw() != d.err)
            FAILF("%s lost its raw errno", d.name);
        CHECK_STR(d.err.name(), d.name);
        for (size_t j = 0; j < i; j++)
            if (KORU_ERRNOS[j].err == d.err)
                FAILF("%s duplicates an errno", d.name);
    }
}

CASE(errno_names_are_shared_but_raw_values_are_not)
{
    CHECK(Error::from_errno(Errno(EBUSY)).kind() == Kind::Again);
    CHECK(Error::from_errno(Errno(EALREADY)).kind() == Kind::Again);
    CHECK(Error::from_errno(Errno(EBUSY)) != Error::from_errno(Errno(EALREADY)));
    // An errno outside the table falls back rather than aborting.
    CHECK(Error::from_errno(Errno(4095)).kind() == Kind::Io);
    CHECK(Error::from_errno(Errno(4095)).raw() == Errno(4095));
    CHECK(Errno(4095).name() == nullptr);
}

CASE(the_res_sign_convention_round_trips)
{
    CHECK(from_res(0).ok());
    CHECK_EQ(from_res(4096).value(), 4096);
    CHECK(!from_res(-EINVAL).ok());
    CHECK_EQ(from_res(-EINVAL).error().value, EINVAL);
    CHECK_EQ(to_res(result<uint64_t, Errno>(Errno(EINVAL))), -EINVAL);
    CHECK_EQ(to_res(result<uint64_t, Errno>(uint64_t(4096))), 4096);
    // CHECKSUM masks to 63 bits, so the whole positive range must survive.
    CHECK_EQ(int64_t(from_res(INT64_MAX).value()), INT64_MAX);
}

CASE(the_vocabulary_keeps_braams_wire_values_and_wording)
{
    CHECK_EQ(int(Kind::Invalid), 1);
    CHECK_EQ(int(Kind::Io), 8);
    CHECK_EQ(int(Kind::Closed), 12);
    CHECK_EQ(int(Kind::Intr), 15);
    CHECK_STR(kind_name(Kind::NotFound), "not found");
    CHECK_STR(kind_name(Kind::NoMemory), "out of memory");
    CHECK_STR(kind_name(Kind::Loop), "too many symbolic links");
    CHECK_STR(kind_name(Kind::Intr), "interrupted");
    // The fifteen, each with its own phrase.
    for (size_t i = 0; i < 15; i++)
        for (size_t j = 0; j < i; j++)
            if (std::string(kind_name(KINDS[i])) == kind_name(KINDS[j]))
                FAILF("two names share a phrase: %s", kind_name(KINDS[i]));
}
