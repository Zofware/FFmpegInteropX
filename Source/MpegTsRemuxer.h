#pragma once
#include <winrt/Windows.Storage.Streams.h>
#include <concurrent_queue.h>
#include <ppltasks.h>
#include "RandomAccessStreamWrapper.h"

struct AVPacket;
struct AVFormatContext;
struct AVBSFContext;

class MpegTsRemuxer
{
public:
    MpegTsRemuxer();

    void BeginFile(AVFormatContext* pInputContext, winrt::Windows::Storage::Streams::IRandomAccessStream const& destStream);
    void WritePacket(AVPacket* pPacket);
    winrt::Windows::Foundation::IAsyncAction EndFile();

    winrt::event_token DurationChanged(winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::TimeSpan> const& handler);
    void DurationChanged(winrt::event_token const& token) noexcept;

    winrt::Windows::Foundation::TimeSpan Duration() const;

private:
    void FileTaskFunc(winrt::Windows::Storage::Streams::IRandomAccessStream destStream);
    void Cleanup();

    bool _isRemuxing = false;

    winrt::event<winrt::Windows::Foundation::EventHandler<winrt::Windows::Foundation::TimeSpan>> _durationChangedEvent;
    winrt::Windows::Foundation::TimeSpan _duration {};

    concurrency::concurrent_queue<AVPacket*> _packetQueue;
    concurrency::event _newPacketEvent;   // set when new packet added to queue

    concurrency::task<void> _fileTask;

    AVFormatContext* _pOutputContext = nullptr;
    AVBSFContext* _pBsfContext = nullptr;
    std::vector<int> _streamMapping;
    std::vector<AVRational> _inputStreamTimeBase;
    RandomAccessStreamWrapper _destStreamWrapper;
    int _iVideoStreamIndex = -1;
};

