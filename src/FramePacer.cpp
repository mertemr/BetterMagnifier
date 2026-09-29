// =============================================================================
// FramePacer.cpp — the render loop's brake, and how it waits
// =============================================================================

#include "pch.h"
#include "FramePacer.h"
#include "Logger.h"

#include <algorithm>

namespace BetterMagnifier {

namespace {

// One second, in the units the interval arithmetic works in.
constexpr long long kNanosPerSecond = 1000000000LL;

// What a display plausibly runs at. Anything outside this is a value we did
// not understand rather than a rate to honour: EnumDisplaySettings reports 0
// or 1 for "the hardware default" on some drivers, and pacing to 1 Hz would
// look exactly like a frozen application.
constexpr unsigned kMinPlausibleRefresh = 24;
constexpr unsigned kMaxPlausibleRefresh = 1000;
constexpr unsigned kFallbackRefresh     = 60;

} // namespace

unsigned ResolveFrameRateCap(unsigned maxFps, unsigned refreshRate)
{
    if (maxFps != 0)
        return std::clamp(maxFps, kMinFrameRateCap, kMaxFrameRateCap);

    if (refreshRate < kMinPlausibleRefresh || refreshRate > kMaxPlausibleRefresh)
        return kFallbackRefresh;

    return refreshRate;
}

// =============================================================================
// FramePacer
// =============================================================================
void FramePacer::SetCap(unsigned fps)
{
    if (fps == m_fps)
        return;

    m_fps = fps;
    m_interval = (fps == 0)
               ? duration::zero()
               : std::chrono::duration_cast<duration>(
                     std::chrono::nanoseconds(kNanosPerSecond / static_cast<long long>(fps)));

    // A new cadence starts fresh rather than inheriting the old deadline,
    // which after a drop from 240 to 30 would otherwise hold the next frame
    // back by a whole 240 Hz interval for no reason.
    Reset();
}

bool FramePacer::Due(time_point now) const
{
    if (m_interval == duration::zero())
        return true;

    // Nothing serviced yet: the first frame must never be held back.
    if (m_next == time_point{})
        return true;

    return now + Tolerance() >= m_next;
}

FramePacer::duration FramePacer::TimeUntilDue(time_point now) const
{
    if (Due(now))
        return duration::zero();

    return m_next - Tolerance() - now;
}

void FramePacer::Serviced(time_point now)
{
    if (m_interval == duration::zero())
    {
        m_next = now;
        return;
    }

    if (m_next == time_point{})
    {
        m_next = now + m_interval;
        return;
    }

    // Advance by exactly one interval, so a present that landed a fraction
    // early or late does not drag the cadence with it.
    m_next += m_interval;

    // Unless we fell a whole frame behind — a GPU hitch, a game taking the
    // output, the machine coming back from a lock. Advancing by one interval
    // would then leave the deadline in the past and the loop would present as
    // fast as it could until it caught up, which is a burst of frames nobody
    // asked for at exactly the moment the machine is already struggling.
    if (m_next < now)
        m_next = now + m_interval;
}

void FramePacer::Reset()
{
    m_next = time_point{};
}

// =============================================================================
// FrameWaiter
// =============================================================================

// Windows 10 1803. Defined here rather than assumed, because the constant is
// absent from older SDK headers and its absence is a build break rather than
// a runtime one.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

FrameWaiter::FrameWaiter()
{
    m_timer = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);

    if (m_timer)
    {
        m_highResolution = true;
    }
    else
    {
        // Pre-1803: the flag is rejected outright, so ask again without it.
        const DWORD err = GetLastError();
        m_timer = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_MANUAL_RESET,
            TIMER_MODIFY_STATE | SYNCHRONIZE);

        LOG_WARN("High-resolution waitable timer unavailable ({}); frame pacing "
                 "falls back to the system timer granularity", err);
    }
}

FrameWaiter::~FrameWaiter()
{
    if (m_timer)
    {
        CloseHandle(m_timer);
        m_timer = nullptr;
    }
}

void FrameWaiter::WaitOrMessage(std::chrono::steady_clock::duration d)
{
    if (d <= std::chrono::steady_clock::duration::zero())
        return;

    const long long ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();

    if (m_timer)
    {
        LARGE_INTEGER due{};
        // Negative is relative, in 100 ns units. Round up, so a sub-tick wait
        // is a wait rather than an immediate return that spins the loop.
        due.QuadPart = -((ns + 99) / 100);
        if (due.QuadPart == 0)
            due.QuadPart = -1;

        if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
        {
            // MWMO_INPUTAVAILABLE so a message that arrived between the last
            // PeekMessage drain and this call is not slept through. The queue
            // is empty by construction at this point — the caller drains it
            // before rendering — so this cannot turn into a spin.
            //
            // The timeout is a backstop, not the mechanism: the timer is what
            // is being waited on. INFINITE would be correct right up until a
            // timer that never signals, and the cost of being wrong about that
            // is a hung application rather than a late frame.
            const DWORD backstopMs =
                static_cast<DWORD>(std::clamp<long long>((ns / 1000000) + 50, 50, 1000));

            MsgWaitForMultipleObjectsEx(1, &m_timer, backstopMs,
                                        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            CancelWaitableTimer(m_timer);
            return;
        }
    }

    // Fallback: as coarse as the system timer, but still woken by a message.
    const long long ms = (ns + 999999) / 1000000;
    MsgWaitForMultipleObjectsEx(
        0, nullptr,
        static_cast<DWORD>(std::clamp<long long>(ms, 1, 16)),
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}

#ifdef _DEBUG
// =============================================================================
// FramePacerSelfCheck
// =============================================================================
// The pacer is pure math with `now` passed in, so the whole cadence — including
// the cases that only happen once a machine is already in trouble — can be
// driven from synthetic timestamps.
// =============================================================================
void FramePacerSelfCheck()
{
    using namespace std::chrono;
    using tp = FramePacer::time_point;

    const tp base = tp{} + hours(1);   // far from the epoch Reset() uses

    const auto intervalFor = [](unsigned fps) {
        return duration_cast<FramePacer::duration>(
            nanoseconds(kNanosPerSecond / static_cast<long long>(fps)));
    };

    // ── ResolveFrameRateCap ──
    {
        // Auto follows the display.
        BM_SELFCHECK(ResolveFrameRateCap(0, 60)  == 60);
        BM_SELFCHECK(ResolveFrameRateCap(0, 144) == 144);
        BM_SELFCHECK(ResolveFrameRateCap(0, 240) == 240);

        // A refresh rate we did not understand is 60, not 0 and not 1.
        BM_SELFCHECK(ResolveFrameRateCap(0, 0) == 60);
        BM_SELFCHECK(ResolveFrameRateCap(0, 1) == 60);
        BM_SELFCHECK(ResolveFrameRateCap(0, 100000) == 60);

        // An explicit override is honoured even above the display's rate:
        // overriding a misreported mode is what it is for.
        BM_SELFCHECK(ResolveFrameRateCap(120, 60) == 120);
        BM_SELFCHECK(ResolveFrameRateCap(30, 144) == 30);

        // ...but not into absurdity.
        BM_SELFCHECK(ResolveFrameRateCap(1, 60) == kMinFrameRateCap);
        BM_SELFCHECK(ResolveFrameRateCap(999999, 60) == kMaxFrameRateCap);
    }

    // ── Uncapped: always due, never waits ──
    {
        FramePacer p;
        p.SetCap(0);
        BM_SELFCHECK(p.Due(base));
        p.Serviced(base);
        BM_SELFCHECK(p.Due(base));
        BM_SELFCHECK(p.TimeUntilDue(base) == FramePacer::duration::zero());
    }

    // ── The first frame is never held back ──
    {
        FramePacer p;
        p.SetCap(60);
        BM_SELFCHECK(p.Due(base));
        BM_SELFCHECK(p.TimeUntilDue(base) == FramePacer::duration::zero());
    }

    // ── A 144 Hz cadence, driven exactly on the deadline ──
    //
    // The regression this guards is the one that makes a 144 Hz panel run at
    // 72: if the deadline is compared without tolerance, a present that lands
    // a hair early is refused and the frame slips to the next interval.
    {
        FramePacer p;
        p.SetCap(144);
        const auto interval = intervalFor(144);

        tp now = base;
        p.Serviced(now);
        BM_SELFCHECK(!p.Due(now));

        for (int i = 0; i < 100; ++i)
        {
            now += interval;
            BM_SELFCHECK(p.Due(now));
            p.Serviced(now);
            BM_SELFCHECK(!p.Due(now));
        }

        // 100 intervals in, the deadline must still be one interval out — not
        // drifted by an accumulation of rounding.
        BM_SELFCHECK(p.TimeUntilDue(now) > FramePacer::duration::zero());
        BM_SELFCHECK(p.TimeUntilDue(now) <= interval);
    }

    // ── Arriving fractionally early is still due ──
    {
        FramePacer p;
        p.SetCap(60);
        const auto interval = intervalFor(60);

        p.Serviced(base);
        // A whole interval minus one twentieth: inside the tolerance band.
        BM_SELFCHECK(p.Due(base + interval - interval / 20));
        // Half an interval out is not.
        BM_SELFCHECK(!p.Due(base + interval / 2));
    }

    // ── Falling behind does not produce a catch-up burst ──
    //
    // A GPU hitch or a lock screen leaves the deadline far in the past.
    // Advancing by one interval each time would let the loop present as fast
    // as it could until it caught up; the resync is what stops that.
    {
        FramePacer p;
        p.SetCap(60);
        const auto interval = intervalFor(60);

        p.Serviced(base);

        const tp late = base + seconds(5);
        BM_SELFCHECK(p.Due(late));
        p.Serviced(late);

        // One interval after the late present, not five seconds' worth of
        // backlog.
        BM_SELFCHECK(!p.Due(late));
        BM_SELFCHECK(!p.Due(late + interval / 2));
        BM_SELFCHECK(p.Due(late + interval));
    }

    // ── Changing the cap restarts the cadence ──
    {
        FramePacer p;
        p.SetCap(30);
        p.Serviced(base);
        BM_SELFCHECK(!p.Due(base));

        p.SetCap(144);
        BM_SELFCHECK(p.Cap() == 144);
        BM_SELFCHECK(p.Due(base));          // not holding a 30 Hz deadline
    }

    // ── Setting the same cap does not disturb the cadence ──
    {
        FramePacer p;
        p.SetCap(60);
        p.Serviced(base);
        BM_SELFCHECK(!p.Due(base));
        p.SetCap(60);
        BM_SELFCHECK(!p.Due(base));
    }

    // ── Reset makes the next frame immediate ──
    {
        FramePacer p;
        p.SetCap(60);
        p.Serviced(base);
        BM_SELFCHECK(!p.Due(base));
        p.Reset();
        BM_SELFCHECK(p.Due(base));
    }

    // ── TimeUntilDue never exceeds the interval, at any rate ──
    {
        for (unsigned fps : { 10u, 24u, 30u, 60u, 75u, 120u, 144u, 165u, 240u, 360u })
        {
            FramePacer p;
            p.SetCap(fps);
            const auto interval = intervalFor(fps);

            p.Serviced(base);
            const auto wait = p.TimeUntilDue(base);
            BM_SELFCHECK(wait > FramePacer::duration::zero());
            BM_SELFCHECK(wait <= interval);

            // And waiting exactly that long gets there.
            BM_SELFCHECK(p.Due(base + wait));
        }
    }

    LOG_INFO("FramePacer self-check passed");
}
#endif // _DEBUG

} // namespace BetterMagnifier
