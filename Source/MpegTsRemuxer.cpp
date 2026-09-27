#include "pch.h"
#include "MpegTsRemuxer.h"

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

#include "RandomAccessStreamWrapper.h"

#include <pplawait.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

MpegTsRemuxer::MpegTsRemuxer()
{
}

static std::wstring FormatAvError(const wchar_t* prefix, int errnum)
{
    // These statements are cascaded instead of combined to aid debugging and setting breakpoints.
    char errorString[1024];
    if (av_strerror(errnum, errorString, sizeof(errorString)) < 0)
    {
        if (0 != strerror_s(errorString, AVUNERROR(errnum)))
        {
            if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, AVUNERROR(errnum), 0, errorString, sizeof(errorString), nullptr) == 0)
            {
                strcpy_s(errorString, "Unknown error");
            }
        }
    }

    wchar_t errorDescription[1024];
    _snwprintf_s(errorDescription, _TRUNCATE, L"%S (%s: %d)", errorString, prefix, errnum);

    return std::wstring(errorDescription);
}

void MpegTsRemuxer::BeginFile(AVFormatContext* pInputContext, IRandomAccessStream const& destStream)
{
    try
    {
        int err = 0;

        if (_pOutputContext != nullptr || _pBsfContext != nullptr)
        {
            throw winrt::hresult_illegal_method_call(L"Remux already in progress");
        }

        // allocate output buffer
        size_t bufferSize = 32768;
        unsigned char* avioOutputBuffer = (unsigned char*)av_malloc(bufferSize);
        if (avioOutputBuffer == nullptr)
        {
            throw winrt::hresult_error(E_OUTOFMEMORY, L"av_malloc");
        }

        _destStreamWrapper = RandomAccessStreamWrapper(destStream);

        // init output AVIOContext
        AVIOContext* avioOutputContext = avio_alloc_context(
            avioOutputBuffer,
            (int)bufferSize,
            1, // write_flag
            &_destStreamWrapper,
            RandomAccessStreamWrapper::read_packet,
            RandomAccessStreamWrapper::write_packet,
            RandomAccessStreamWrapper::seek);

        if (avioOutputContext == nullptr)
        {
            throw winrt::hresult_error(E_OUTOFMEMORY, L"avio_alloc_context");
        }

        // output
        if ((err = avformat_alloc_output_context2(&_pOutputContext, nullptr, "mpegts", nullptr)) < 0)
        {
            throw winrt::hresult_error(E_OUTOFMEMORY, FormatAvError(L"avformat_alloc_output_context2", err).c_str());
        }

        _pOutputContext->pb = avioOutputContext;

        _streamMapping.resize(pInputContext->nb_streams);
        _inputStreamTimeBase.resize(pInputContext->nb_streams);

        int iStreamIndex = 0;
        _iVideoStreamIndex = -1;
        for (unsigned int i = 0; i < pInputContext->nb_streams; i++)
        {
            // Create output AVStream according to input AVStream
            AVStream* pInputStream = pInputContext->streams[i];
            AVCodecParameters* pInputCodecParams = pInputStream->codecpar;

            if (pInputCodecParams->codec_type != AVMEDIA_TYPE_AUDIO &&
                pInputCodecParams->codec_type != AVMEDIA_TYPE_VIDEO &&
                pInputCodecParams->codec_type != AVMEDIA_TYPE_SUBTITLE)
            {
                _streamMapping[i] = -1;
                continue;
            }

            _streamMapping[i] = iStreamIndex++;
            _inputStreamTimeBase[i] = pInputStream->time_base;

            if (pInputCodecParams->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                _iVideoStreamIndex = _streamMapping[i];
            }


            AVStream* pOutputStream = avformat_new_stream(_pOutputContext, nullptr);
            if (!pOutputStream)
            {
                throw winrt::hresult_error(E_OUTOFMEMORY, L"avformat_new_stream");
            }

            AVCodecParameters* pOutputCodecParams = pOutputStream->codecpar;
            if ((err = avcodec_parameters_copy(pOutputCodecParams, pInputCodecParams)) < 0)
            {
                throw winrt::hresult_error(E_FAIL, FormatAvError(L"avcodec_parameters_copy", err).c_str());
            }

            pOutputStream->codecpar->codec_tag = 0;
        }

        av_dump_format(_pOutputContext, 0, "output", 1);

        // Use h264_mp4toannexb bitstream filter to convert h264 from mp4 format to annexb format, which is required by mpegts muxer
        if (_iVideoStreamIndex >= 0)
        {
            AVCodecParameters* pVideoCodecParams = pInputContext->streams[_iVideoStreamIndex]->codecpar;

            // 1. Find the filter by name
            const AVBitStreamFilter* bsf = av_bsf_get_by_name("h264_mp4toannexb");
            if (!bsf)
            {
                throw winrt::hresult_error(E_FAIL, L"av_bsf_get_by_name");
            }

            // 2. Allocate the BSF context
            if ((err = av_bsf_alloc(bsf, &_pBsfContext)) < 0)
            {
                throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_bsf_alloc", err).c_str());
            }

            // 3. Copy codec parameters from the input stream to the BSF context
            if ((err = avcodec_parameters_copy(_pBsfContext->par_in, pVideoCodecParams)) < 0)
            {
                throw winrt::hresult_error(E_FAIL, FormatAvError(L"avcodec_parameters_copy", err).c_str());
            }

            // 4. Initialize the filter context
            if ((err = av_bsf_init(_pBsfContext)) < 0)
            {
                throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_bsf_init", err).c_str());
            }
        }

        // Write file header
        if ((err = avformat_write_header(_pOutputContext, nullptr)) < 0)
        {
            throw winrt::hresult_error(E_FAIL, FormatAvError(L"avformat_write_header", err).c_str());
        }
    }
    catch (...)
    {
        if (_pOutputContext != nullptr)
        {
            avformat_free_context(_pOutputContext);
            _pOutputContext = nullptr;
        }

        if (_pBsfContext != nullptr)
        {
            av_bsf_free(&_pBsfContext);
            _pBsfContext = nullptr;
        }

        throw;
    }

    _isRemuxing = true;

    _fileTask = concurrency::create_task([destStream, this]
    {
        FileTaskFunc(destStream);
    });
}

void MpegTsRemuxer::WritePacket(AVPacket* packet)
{
    if (_isRemuxing)
    {
        auto* clone = (packet != nullptr) ? av_packet_clone(packet) : nullptr;
        _packetQueue.push(clone);
        _newPacketEvent.set();
    }
}

winrt::Windows::Foundation::IAsyncAction MpegTsRemuxer::EndFile()
{
    if (_isRemuxing)
    {
        _packetQueue.push(nullptr); // Signal end of file

        if (!_fileTask.is_done())
        {
            co_await _fileTask;
        }
    }

    co_return;
}

// Define the WinRT time base: 1 tick = 100 nanoseconds
// This means there are 10,000,000 ticks in 1 second
const AVRational winrtTimeBase = { 1, 10000000 };

winrt::event_token MpegTsRemuxer::DurationChanged(EventHandler<TimeSpan> const& handler)
{
    return _durationChangedEvent.add(handler);
}

void MpegTsRemuxer::DurationChanged(winrt::event_token const& token) noexcept
{
    _durationChangedEvent.remove(token);
}

TimeSpan MpegTsRemuxer::Duration() const
{
    return _duration;
}

void MpegTsRemuxer::FileTaskFunc(IRandomAccessStream destStream)
{
    int err = 0;
    try
    {
        while (1)
        {
            // Get an AVPacket
            AVPacket* pPacket = nullptr;
            while (1)
            {
                if (_packetQueue.try_pop(pPacket))
                {
                    break;
                }

                _newPacketEvent.wait();
                _newPacketEvent.reset();
            }

            if (pPacket == nullptr)
            {
                break;
            }

            if (pPacket->stream_index < _streamMapping.size() &&
                _streamMapping[pPacket->stream_index] >= 0)
            {
                pPacket->stream_index = _streamMapping[pPacket->stream_index];
                AVStream* pOutputStream = _pOutputContext->streams[pPacket->stream_index];

                auto inputStreamTimeBase = _inputStreamTimeBase[pPacket->stream_index];
                auto outputStreamTimeBase = pOutputStream->time_base;

                // adjust timing information
                pPacket->pts = av_rescale_q_rnd(pPacket->pts, inputStreamTimeBase, outputStreamTimeBase, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                pPacket->dts = av_rescale_q_rnd(pPacket->dts, inputStreamTimeBase, outputStreamTimeBase, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                pPacket->duration = av_rescale_q(pPacket->duration, inputStreamTimeBase, outputStreamTimeBase);
                pPacket->pos = -1;

                // Rescale the packet duration from stream time base to WinRT time base
                int64_t ticks = av_rescale_q(pPacket->duration, pOutputStream->time_base, winrtTimeBase);
                TimeSpan pktDuration{ std::chrono::duration<int64_t, std::ratio<1, 10000000>>(ticks) };

                if (pPacket->stream_index == _iVideoStreamIndex)
                {
                    // Push the MP4/AVCC packet into the filter
                    if ((err = av_bsf_send_packet(_pBsfContext, pPacket)) < 0)
                    {
                        throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_bsf_send_packet", err).c_str());
                    }

                    // Pull the Annex B packet out of the filter
                    // Note: One input packet can sometimes yield multiple or zero output packets
                    AVPacket filteredPkt;
                    while ((err = av_bsf_receive_packet(_pBsfContext, &filteredPkt)) == 0)
                    {
                        // filteredPkt now contains the Annex B data (with start codes)
                        err = av_interleaved_write_frame(_pOutputContext, &filteredPkt);
                        av_packet_unref(&filteredPkt);

                        if (err < 0)
                        {
                            av_packet_unref(pPacket);
                            throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_interleaved_write_frame", err).c_str());
                        }
                    }

                    av_packet_unref(pPacket);

                    if (err != AVERROR(EAGAIN) && err != AVERROR_EOF)
                    {
                        throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_bsf_receive_packet", err).c_str());
                    }

                    _duration += pktDuration;
                    _durationChangedEvent(destStream, _duration);
                }
                else
                {
                    err = av_interleaved_write_frame(_pOutputContext, pPacket);
                    av_packet_unref(pPacket);

                    if (err < 0)
                    {
                        throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_interleaved_write_frame", err).c_str());
                    }
                }
            }
        }

        // Write file trailer
        if ((err = av_write_trailer(_pOutputContext)) != 0)
        {
            throw winrt::hresult_error(E_FAIL, FormatAvError(L"av_write_trailer", err).c_str());
        }
    }
    catch (...)
    {
        // clean up
        if (destStream != nullptr)
        {
            destStream.FlushAsync();
        }

        Cleanup();

        throw;
    }
}

void MpegTsRemuxer::Cleanup()
{
    _isRemuxing = false;

    while (!_packetQueue.empty())
    {
        AVPacket* pPacket = nullptr;
        if (_packetQueue.try_pop(pPacket))
        {
            if (pPacket != nullptr)
            {
                av_packet_unref(pPacket);
            }
        }
    }

    if (_pOutputContext != nullptr)
    {
        avformat_free_context(_pOutputContext);
        _pOutputContext = nullptr;
    }

    if (_pBsfContext != nullptr)
    {
        av_bsf_free(&_pBsfContext);
        _pBsfContext = nullptr;
    }

    _streamMapping.clear();
    _inputStreamTimeBase.clear();
    _destStreamWrapper = RandomAccessStreamWrapper();
    _iVideoStreamIndex = -1;
}
