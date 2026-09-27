#pragma once

#include <stdint.h>
#include <winrt/Windows.Storage.Streams.h>

class RandomAccessStreamWrapper
{
public:
    RandomAccessStreamWrapper();
    RandomAccessStreamWrapper(winrt::Windows::Storage::Streams::IRandomAccessStream const& stream);

    static int read_packet(void* pThis, uint8_t* buf, int buf_size);
    static int write_packet(void* pThis, const uint8_t* buf, int buf_size);
    static int64_t seek(void* pThis, int64_t offset, int whence);

    int ReadPacket(uint8_t* buf, int buf_size);
    int WritePacket(const uint8_t* buf, int buf_size);
    int64_t Seek(int64_t offset, int whence);

private:
    winrt::Windows::Storage::Streams::IRandomAccessStream _stream = nullptr;
    winrt::Windows::Storage::Streams::IBuffer _buffer = nullptr;

    winrt::Windows::Storage::Streams::IBuffer GetBuffer(int buf_size);
    void RecycleBuffer(winrt::Windows::Storage::Streams::IBuffer const& buffer);
};
