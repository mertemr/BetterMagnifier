// Desktop Duplication capture.
//
// COM release order matters here. The duplication session must go before the
// output reference, and the device and context are NOT ours to release:
// D3DRenderer owns them and we only borrow raw pointers.

#include "pch.h"
#include "DXGICapture.h"
#include "Logger.h"

namespace BetterMagnifier {

namespace {

// How long to wait between recovery attempts. While the workstation is locked
// every DuplicateOutput call fails, so retrying per frame only spams the log.
constexpr auto kReinitInterval = std::chrono::milliseconds(500);

const char* RotationName(DXGI_MODE_ROTATION rotation)
{
    switch (rotation)
    {
    case DXGI_MODE_ROTATION_IDENTITY:  return "none";
    case DXGI_MODE_ROTATION_ROTATE90:  return "90";
    case DXGI_MODE_ROTATION_ROTATE180: return "180";
    case DXGI_MODE_ROTATION_ROTATE270: return "270";
    default:                           return "unspecified";
    }
}

} // namespace

DXGICapture::~DXGICapture()
{
    Cleanup();
}

DXGICapture::DXGICapture(DXGICapture&& other) noexcept
    : m_duplication(std::move(other.m_duplication))
    , m_output1(std::move(other.m_output1))
    , m_device(other.m_device)
    , m_context(other.m_context)
    , m_frameAcquired(other.m_frameAcquired)
    , m_initialized(other.m_initialized)
    , m_needsReinit(other.m_needsReinit)
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_rotation(other.m_rotation)
    , m_frameCount(other.m_frameCount)
    , m_errorCount(other.m_errorCount)
    , m_firstFrame(other.m_firstFrame)
    , m_metadata(std::move(other.m_metadata))
    , m_dirtyRects(std::move(other.m_dirtyRects))
    , m_dirtyKnown(other.m_dirtyKnown)
    , m_lastReinitAttempt(other.m_lastReinitAttempt)
{
    other.m_device = nullptr;
    other.m_context = nullptr;
    other.m_frameAcquired = false;
    other.m_initialized = false;
}

DXGICapture& DXGICapture::operator=(DXGICapture&& other) noexcept
{
    if (this != &other)
    {
        Cleanup();
        m_duplication       = std::move(other.m_duplication);
        m_output1           = std::move(other.m_output1);
        m_device            = other.m_device;
        m_context           = other.m_context;
        m_frameAcquired     = other.m_frameAcquired;
        m_initialized       = other.m_initialized;
        m_needsReinit       = other.m_needsReinit;
        m_width             = other.m_width;
        m_height            = other.m_height;
        m_rotation          = other.m_rotation;
        m_frameCount        = other.m_frameCount;
        m_errorCount        = other.m_errorCount;
        m_firstFrame        = other.m_firstFrame;
        m_metadata          = std::move(other.m_metadata);
        m_dirtyRects        = std::move(other.m_dirtyRects);
        m_dirtyKnown        = other.m_dirtyKnown;
        m_lastReinitAttempt = other.m_lastReinitAttempt;

        other.m_device = nullptr;
        other.m_context = nullptr;
        other.m_frameAcquired = false;
        other.m_initialized = false;
    }
    return *this;
}

bool DXGICapture::Initialize(ID3D11Device* device, IDXGIOutput* output)
{
    if (!device || !output)
    {
        LOG_ERROR("DXGICapture::Initialize - device or output is null");
        return false;
    }

    Cleanup();

    m_device = device;
    device->GetImmediateContext(&m_context);

    // Desktop Duplication lives on IDXGIOutput1 and above.
    HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&m_output1));
    if (FAILED(hr))
    {
        LOG_ERROR("IDXGIOutput1 QueryInterface failed: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    // DesktopCoordinates, so m_width/m_height are DESKTOP dimensions. That is
    // what the rest of the application works in, and on a rotated output it is
    // not the size of the texture AcquireFrame returns — the panel's own,
    // unrotated resolution, with the axes the other way round. The renderer is
    // told the rotation and resolves the difference there.
    DXGI_OUTPUT_DESC outputDesc{};
    m_output1->GetDesc(&outputDesc);
    m_width  = static_cast<UINT>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
    m_height = static_cast<UINT>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
    m_rotation = outputDesc.Rotation;

    // Subscribes to the monitor's desktop composition.
    //
    // Expected failures:
    //   E_ACCESSDENIED                     secure desktop (lock screen, UAC)
    //   DXGI_ERROR_NOT_CURRENTLY_AVAILABLE another process holds duplication
    //   DXGI_ERROR_UNSUPPORTED             GPU or driver cannot do it
    hr = m_output1->DuplicateOutput(m_device, &m_duplication);
    if (FAILED(hr))
    {
        if (hr == E_ACCESSDENIED)
            LOG_WARN("DuplicateOutput: E_ACCESSDENIED (locked or secure desktop), will retry");
        else if (hr == static_cast<HRESULT>(DXGI_ERROR_NOT_CURRENTLY_AVAILABLE))
            LOG_WARN("DuplicateOutput: DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (another app holds it), will retry");
        else if (hr == static_cast<HRESULT>(DXGI_ERROR_UNSUPPORTED))
            LOG_ERROR("DuplicateOutput: DXGI_ERROR_UNSUPPORTED (GPU or driver)");
        else
            LOG_ERROR("DuplicateOutput failed: 0x{:08X}", static_cast<unsigned long>(hr));

        // Deliberately NOT Cleanup(): that would null the device and reset the
        // output, and Reinitialize needs both. Starting up while the machine is
        // locked used to kill capture permanently for exactly that reason.
        m_initialized = false;
        m_needsReinit = true;
        return false;
    }

    m_initialized = true;
    m_needsReinit = false;
    m_frameCount  = 0;
    m_errorCount  = 0;
    m_firstFrame  = true;
    m_dirtyKnown  = false;
    m_dirtyRects.clear();

    LOG_INFO("DXGICapture started: {}x{} desktop, rotation {}",
        m_width, m_height, RotationName(m_rotation));
    return true;
}

// Timeout 0 means ask without blocking: take a new frame if there is one,
// otherwise return immediately. Blocking is wrong here because the anchor can
// move while the screen is static, and then the previous frame has to be
// re-presented against a new source region.
CapturedFrame DXGICapture::AcquireFrame(UINT timeoutMs)
{
    CapturedFrame result{};

    if (!m_initialized || !m_duplication)
    {
        m_needsReinit = true;
        return result;
    }

    // Release a frame the caller forgot about, or the next acquire fails.
    if (m_frameAcquired)
        ReleaseFrame();

    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo{};

    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // Normal: the screen did not change. Which also means "what changed"
        // is known and empty, rather than left over from the last frame that
        // did arrive — callers guard on isNewFrame first, but a stale answer
        // sitting in an accessor is a trap for the next one that does not.
        result.isNewFrame = false;
        m_dirtyKnown = true;
        m_dirtyRects.clear();
        return result;
    }

    // ACCESS_LOST on a desktop switch (fullscreen exclusive app, lock screen),
    // ACCESS_DENIED once the secure desktop is up.
    if (hr == static_cast<HRESULT>(DXGI_ERROR_ACCESS_LOST) || hr == E_ACCESSDENIED)
    {
        LOG_WARN("Duplication lost (0x{:08X}), will recover", static_cast<unsigned long>(hr));
        ReleaseDuplication();
        return result;
    }

    if (FAILED(hr))
    {
        m_errorCount++;
        if (m_errorCount % 100 == 1)   // log one in a hundred, not a flood
        {
            LOG_WARN("AcquireNextFrame failed: 0x{:08X} (error #{})",
                static_cast<unsigned long>(hr), m_errorCount);
        }

        // Anything unexpected also goes through recovery. Leaving the session
        // in place would keep failing forever with no way back.
        ReleaseDuplication();
        return result;
    }

    m_frameAcquired = true;

    hr = desktopResource.As(&result.texture);
    if (FAILED(hr) || !result.texture)
    {
        LOG_ERROR("IDXGIResource to ID3D11Texture2D failed");
        ReleaseFrame();
        return result;
    }

    // ── Was the desktop image actually updated? ──
    //
    // AcquireNextFrame succeeds for a pointer-only update too, and says so by
    // leaving LastPresentTime at zero: the texture handed back is the previous
    // desktop image, unchanged. Treating that as a new frame cost a
    // full-screen copy, a full shader pass and a layered-window Present on
    // every mouse move over a static screen — which, for a magnifier, is the
    // single most common thing that happens.
    //
    // The first frame after DuplicateOutput is forced through regardless. It
    // carries the whole desktop, and on a screen that is not changing there
    // may never be another one; without this the magnifier would come up
    // showing nothing at all until something moved.
    const bool desktopUpdated = (frameInfo.LastPresentTime.QuadPart != 0) || m_firstFrame;

    if (desktopUpdated)
    {
        if (m_firstFrame)
        {
            // No metadata is trustworthy for a frame we are forcing through.
            m_firstFrame = false;
            m_dirtyKnown = false;
            m_dirtyRects.clear();
        }
        else
        {
            ReadDirtyMetadata(frameInfo);
        }
    }
    else
    {
        // Nothing repainted, so "what changed" is the empty set — not unknown.
        m_dirtyKnown = true;
        m_dirtyRects.clear();
    }

    result.frameInfo  = frameInfo;
    result.isNewFrame = desktopUpdated;
    result.width      = m_width;
    result.height     = m_height;

    if (desktopUpdated)
        m_frameCount++;

    return result;
}

// =============================================================================
// ReadDirtyMetadata — which parts of the texture this frame repainted
// =============================================================================
//
// Move rects have to be read before dirty rects: they share one metadata
// buffer and the dirty rects follow the move rects in it.
//
// A move is a scroll — a region copied from one place on screen to another.
// Both ends are treated as changed: the destination obviously, and the source
// because whatever was uncovered behind it is now something else. Being
// conservative here costs a skipped optimisation; being wrong costs a stale
// region on screen with nothing to make it repaint.
//
// Any failure — no metadata, a short buffer, an error — leaves m_dirtyKnown
// false, which the caller reads as "assume everything changed". There is no
// path here that can make the renderer skip a region that really did move.
// =============================================================================
void DXGICapture::ReadDirtyMetadata(const DXGI_OUTDUPL_FRAME_INFO& frameInfo)
{
    m_dirtyKnown = false;
    m_dirtyRects.clear();

    if (!m_duplication || frameInfo.TotalMetadataBufferSize == 0)
        return;

    if (m_metadata.size() < frameInfo.TotalMetadataBufferSize)
        m_metadata.resize(frameInfo.TotalMetadataBufferSize);

    const UINT bufferSize = static_cast<UINT>(m_metadata.size());
    UINT used = 0;

    // ── Move rects ──
    UINT moveBytes = 0;
    HRESULT hr = m_duplication->GetFrameMoveRects(
        bufferSize, reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(m_metadata.data()), &moveBytes);

    if (FAILED(hr))
        return;

    const auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(m_metadata.data());
    const size_t moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);

    for (size_t i = 0; i < moveCount; ++i)
    {
        const RECT& dst = moves[i].DestinationRect;
        m_dirtyRects.push_back(dst);

        // The source region, which is the destination rectangle's size taken
        // from wherever it was copied.
        RECT src{};
        src.left   = moves[i].SourcePoint.x;
        src.top    = moves[i].SourcePoint.y;
        src.right  = src.left + (dst.right  - dst.left);
        src.bottom = src.top  + (dst.bottom - dst.top);
        m_dirtyRects.push_back(src);
    }

    used = moveBytes;

    // ── Dirty rects, written after the move rects in the same buffer ──
    UINT dirtyBytes = 0;
    hr = m_duplication->GetFrameDirtyRects(
        bufferSize - used, reinterpret_cast<RECT*>(m_metadata.data() + used), &dirtyBytes);

    if (FAILED(hr))
    {
        m_dirtyRects.clear();
        return;
    }

    const auto* dirty = reinterpret_cast<const RECT*>(m_metadata.data() + used);
    const size_t dirtyCount = dirtyBytes / sizeof(RECT);

    for (size_t i = 0; i < dirtyCount; ++i)
        m_dirtyRects.push_back(dirty[i]);

    m_dirtyKnown = true;
}

// Empty and known means nothing repainted. Empty and unknown never reaches
// here — the caller checks DirtyKnown() first — but returning true for it
// would still be the safe answer, so it does.
bool DXGICapture::DirtyIntersects(const RECT& textureRect) const
{
    if (!m_dirtyKnown)
        return true;

    for (const RECT& r : m_dirtyRects)
    {
        RECT out{};
        if (IntersectRect(&out, &r, &textureRect))
            return true;
    }

    return false;
}

void DXGICapture::ReleaseFrame()
{
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }
}

// Drops the duplication session but keeps device, context and output, which is
// what makes recovery possible.
void DXGICapture::ReleaseDuplication()
{
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    m_duplication.Reset();
    m_initialized = false;
    m_needsReinit = true;
}

bool DXGICapture::Reinitialize()
{
    if (!m_device || !m_output1)
    {
        LOG_ERROR("Reinitialize: device or output is gone, cannot recover");
        m_needsReinit = false;   // hopeless, stop retrying
        return false;
    }

    // Throttle: while locked this fails every time.
    const auto now = std::chrono::steady_clock::now();
    if (m_lastReinitAttempt.time_since_epoch().count() != 0
        && now - m_lastReinitAttempt < kReinitInterval)
    {
        return false;
    }
    m_lastReinitAttempt = now;

    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }
    m_duplication.Reset();

    // Re-read rather than trust what Initialize saw. Rotating a display drops
    // the duplication session and comes back through here; keeping the old
    // orientation would leave the monitor sideways until the next restart.
    DXGI_OUTPUT_DESC outputDesc{};
    m_output1->GetDesc(&outputDesc);
    m_width  = static_cast<UINT>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
    m_height = static_cast<UINT>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
    m_rotation = outputDesc.Rotation;

    HRESULT hr = m_output1->DuplicateOutput(m_device, &m_duplication);
    if (FAILED(hr))
    {
        // Quiet by design: expected on every attempt while the machine is
        // locked, and this runs twice a second.
        LOG_DEBUG("Reinitialize not ready yet: 0x{:08X}", static_cast<unsigned long>(hr));
        return false;
    }

    m_initialized = true;
    m_needsReinit = false;
    m_errorCount  = 0;
    m_firstFrame  = true;
    m_dirtyKnown  = false;
    m_dirtyRects.clear();

    LOG_INFO("DXGICapture recovered ({}x{} desktop, rotation {})",
        m_width, m_height, RotationName(m_rotation));
    return true;
}

void DXGICapture::Cleanup()
{
    if (m_frameAcquired && m_duplication)
    {
        m_duplication->ReleaseFrame();
        m_frameAcquired = false;
    }

    m_duplication.Reset();
    m_output1.Reset();

    // We took this reference with GetImmediateContext, so we release it.
    if (m_context)
    {
        m_context->Release();
        m_context = nullptr;
    }

    // The device belongs to D3DRenderer; only drop our borrowed pointer.
    m_device = nullptr;

    m_initialized = false;

    LOG_DEBUG("DXGICapture cleanup done (frames: {}, errors: {})",
        m_frameCount, m_errorCount);
}

} // namespace BetterMagnifier
