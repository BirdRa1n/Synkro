#pragma once

#include <string>
#include <utility>
#include <variant>

namespace synkro {

// Categoria do erro. Mantida pequena e estável: é usada em eventos, alertas e
// códigos de saída do worker.
enum class ErrorCode {
    InvalidArgument,
    NotFound,
    AccessDenied,
    AlreadyExists,
    IoError,
    DiskFull,
    DeviceNotReady,
    SourceChangedDuringCopy,
    VerificationFailed,
    Corrupted,
    Unsupported,
    KeyRequired,   // cofre cifrado e a chave não está disponível nesta máquina
    Internal,
};

const char* nameOf(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;          // UTF-8, legível por humanos
    unsigned long systemCode = 0; // GetLastError()/NTSTATUS, quando houver

    static Error make(ErrorCode code, std::string message, unsigned long systemCode = 0)
    {
        return Error{code, std::move(message), systemCode};
    }
};

// Resultado de uma operação que pode falhar. Substituto mínimo de
// std::expected (C++23) enquanto o projeto compila em C++20.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_data(std::move(value)) {}
    Result(Error error) : m_data(std::move(error)) {}

    bool ok() const noexcept { return std::holds_alternative<T>(m_data); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<T>(m_data); }
    const T& value() const& { return std::get<T>(m_data); }
    T&& value() && { return std::get<T>(std::move(m_data)); }

    const Error& error() const { return std::get<Error>(m_data); }

private:
    std::variant<T, Error> m_data;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}

    static Result success() { return Result(); }

    bool ok() const noexcept { return m_ok; }
    explicit operator bool() const noexcept { return m_ok; }

    const Error& error() const { return m_error; }

private:
    Error m_error;
    bool m_ok = true;
};

using Status = Result<void>;

} // namespace synkro
