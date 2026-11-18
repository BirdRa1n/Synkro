#pragma once

#include <windows.h>

#include <cstddef>

namespace synkro::win {

// Buffer alocado com VirtualAlloc: alinhado à página (4 KiB), requisito das
// leituras e escritas com FILE_FLAG_NO_BUFFERING.
class AlignedBuffer {
public:
    explicit AlignedBuffer(std::size_t size)
        : m_data(::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
        , m_size(m_data ? size : 0)
    {
    }

    ~AlignedBuffer()
    {
        if (m_data)
            ::VirtualFree(m_data, 0, MEM_RELEASE);
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    void* data() const noexcept { return m_data; }
    std::size_t size() const noexcept { return m_size; }
    explicit operator bool() const noexcept { return m_data != nullptr; }

private:
    void* m_data;
    std::size_t m_size;
};

} // namespace synkro::win
