#include "pch.h"

#include "RandomAccessStreamWrapper.h"
#include <concrt.h>

extern "C"
{
#include <libavformat/avio.h>
}

using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Foundation;

using winrt::com_ptr;

RandomAccessStreamWrapper::RandomAccessStreamWrapper()
    : _stream(nullptr), _buffer(nullptr)
{
}

RandomAccessStreamWrapper::RandomAccessStreamWrapper(IRandomAccessStream const& stream)
    : _stream(stream), _buffer(nullptr)
{
}

IBuffer RandomAccessStreamWrapper::GetBuffer(int buf_size)
{
    IBuffer buffer;
    if (_buffer != nullptr && _buffer.Capacity() >= (unsigned int)buf_size)
    {
        buffer = _buffer;
        _buffer = nullptr;
    }
    else
    {
        buffer = Buffer(buf_size);
    }

    buffer.Length(buf_size);

    return buffer;
}

void RandomAccessStreamWrapper::RecycleBuffer(IBuffer const& buffer)
{
    _buffer = buffer;
}


int RandomAccessStreamWrapper::read_packet(void* pThis, uint8_t* buf, int buf_size)
{
    return static_cast<RandomAccessStreamWrapper*>(pThis)->ReadPacket(buf, buf_size);
}

int RandomAccessStreamWrapper::write_packet(void* pThis, const uint8_t* buf, int buf_size)
{
    return static_cast<RandomAccessStreamWrapper*>(pThis)->WritePacket(buf, buf_size);
}

int64_t RandomAccessStreamWrapper::seek(void* pThis, int64_t offset, int whence)
{
    return static_cast<RandomAccessStreamWrapper*>(pThis)->Seek(offset, whence);
}

int RandomAccessStreamWrapper::ReadPacket(uint8_t* buf, int buf_size)
{
    IBuffer buffer = GetBuffer(buf_size);

    auto asyncOp = _stream.ReadAsync(
        buffer,
        (uint32_t)buf_size,
        InputStreamOptions::None);

    int result = -1;
    if (asyncOp.get() != nullptr && asyncOp.Status() == AsyncStatus::Completed)
    {
        memcpy_s(buf, buf_size, buffer.data(), buffer.Length());
        result = buffer.Length();
    }
    else
    {
        asyncOp.Cancel();
    }

    RecycleBuffer(buffer);

    return result;
}

int RandomAccessStreamWrapper::WritePacket(const uint8_t* buf, int buf_size)
{
    auto buffer = GetBuffer(buf_size);
    memcpy_s(buffer.data(), buffer.Length(), buf, buf_size);

    auto asyncOp = _stream.WriteAsync(buffer);

    int result = asyncOp.get();
    if (asyncOp.Status() != AsyncStatus::Completed)
    {
        result = -1;
    }

    RecycleBuffer(buffer);

    return result;
}

int64_t RandomAccessStreamWrapper::Seek(int64_t offset, int whence)
{
    if (whence & AVSEEK_SIZE)
    {
        return _stream.Position();
    }
    else
    {
        _stream.Seek(offset);
        return _stream.Position();
    }
}
