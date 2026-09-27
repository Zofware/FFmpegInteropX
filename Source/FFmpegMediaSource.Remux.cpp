#include "pch.h"
#include "FFmpegMediaSource.h"
#include "FFmpegReader.h"

namespace winrt::FFmpegInteropX::implementation
{
    using namespace Windows::Foundation;
    using namespace Windows::Storage::Streams;
    using namespace Windows::Media::Playback;

    void FFmpegMediaSource::StartRemuxToMpegTsAsync(IRandomAccessStream outputStream)
    {
        try
        {
            if (isRemuxing)
            {
                throw winrt::hresult_illegal_method_call(L"Remuxing is already running.");
            }

            if (!outputStream)
            {
                throw winrt::hresult_invalid_argument(L"Output stream cannot be null.");
            }

            if (!m_pReader)
            {
                throw winrt::hresult_illegal_method_call(L"FFmpegMediaSource is not initialized.");
            }

            isRemuxing = true;

            remuxer = std::make_unique<MpegTsRemuxer>();
            remuxer->BeginFile(avFormatCtx, outputStream);

            remuxDurationChangedToken = remuxer->DurationChanged([this](auto const&, winrt::Windows::Foundation::TimeSpan const& duration)
                {
                    remuxDurationChangedEvent(*this, duration);
                });

            m_pReader->SetPacketReadCallback([this](AVPacket* packet)
                {
                    remuxer->WritePacket(packet);
                });
        }
        catch (...)
        {
            // clean up
            if (remuxer != nullptr)
            {
                if (remuxDurationChangedToken)
                {
                    remuxer->DurationChanged(remuxDurationChangedToken);
                    remuxDurationChangedToken = {};
                }

                if (m_pReader != nullptr)
                {
                    m_pReader->SetPacketReadCallback(nullptr);
                }

                remuxer->EndFile();
            }
            isRemuxing = false;

            throw;
        }
    }

    winrt::Windows::Foundation::IAsyncAction FFmpegMediaSource::StopRemux()
    {
        if (isRemuxing && remuxer != nullptr)
        {
            if (m_pReader != nullptr)
            {
                m_pReader->SetPacketReadCallback(nullptr);
            }

            if (remuxer != nullptr)
            {
                co_await remuxer->EndFile();
            }

            isRemuxing = false;
        }
    }

    winrt::event_token FFmpegMediaSource::RemuxDurationChanged(winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::TimeSpan> const& handler)
    {
        return remuxDurationChangedEvent.add(handler);
    }

    void FFmpegMediaSource::RemuxDurationChanged(winrt::event_token const& token) noexcept
    {
        remuxDurationChangedEvent.remove(token);
    }

}
