//
//  HTTPRequest
//

#ifndef HTTPREQUEST_HPP
#define HTTPREQUEST_HPP

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#  pragma push_macro("WIN32_LEAN_AND_MEAN")
#  pragma push_macro("NOMINMAX")
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  if _WIN32_WINNT < _WIN32_WINNT_WINXP
extern "C" char *_strdup(const char *strSource);
#    define strdup _strdup
#    include <wspiapi.h>
#  endif
#  include <ws2tcpip.h>
#  pragma pop_macro("WIN32_LEAN_AND_MEAN")
#  pragma pop_macro("NOMINMAX")
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace http
{
    class RequestError final: public std::logic_error
    {
    public:
        explicit RequestError(const char* str): std::logic_error(str) {}
        explicit RequestError(const std::string& str): std::logic_error(str) {}
    };

    class ResponseError final: public std::runtime_error
    {
    public:
        explicit ResponseError(const char* str): std::runtime_error(str) {}
        explicit ResponseError(const std::string& str): std::runtime_error(str) {}
    };

    enum class InternetProtocol: std::uint8_t
    {
        V4,
        V6
    };

    inline namespace detail
    {
#ifdef _WIN32
        class WinSock final
        {
        public:
            WinSock()
            {
                WSADATA wsaData;
                const auto error = WSAStartup(MAKEWORD(2, 2), &wsaData);
                if (error != 0)
                    throw std::system_error(error, std::system_category(), "WSAStartup failed");

                if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
                {
                    WSACleanup();
                    throw std::runtime_error("Invalid WinSock version");
                }

                started = true;
            }

            ~WinSock()
            {
                if (started) WSACleanup();
            }

            WinSock(WinSock&& other) noexcept:
                started(other.started)
            {
                other.started = false;
            }

            WinSock& operator=(WinSock&& other) noexcept
            {
                if (&other == this) return *this;
                if (started) WSACleanup();
                started = other.started;
                other.started = false;
                return *this;
            }

        private:
            bool started = false;
        };
#endif

        inline int getLastError() noexcept
        {
#ifdef _WIN32
            return WSAGetLastError();
#else
            return errno;
#endif
        }

        constexpr int getAddressFamily(InternetProtocol internetProtocol)
        {
            return (internetProtocol == InternetProtocol::V4) ? AF_INET :
                (internetProtocol == InternetProtocol::V6) ? AF_INET6 :
                throw RequestError("Unsupported protocol");
        }

#ifdef _WIN32
        constexpr auto closeSocket = closesocket;
#else
        constexpr auto closeSocket = close;
#endif

#if defined(__unix__) && !defined(__APPLE__)
        constexpr int noSignal = MSG_NOSIGNAL;
#else
        constexpr int noSignal = 0;
#endif

        class Socket final
        {
        public:
#ifdef _WIN32
            using Type = SOCKET;
            static constexpr Type invalid = INVALID_SOCKET;
#else
            using Type = int;
            static constexpr Type invalid = -1;
#endif

            explicit Socket(InternetProtocol internetProtocol):
                endpoint(socket(getAddressFamily(internetProtocol), SOCK_STREAM, IPPROTO_TCP))
            {
                if (endpoint == invalid)
                    throw std::system_error(getLastError(), std::system_category(), "Failed to create socket");

#ifdef _WIN32
                unsigned long mode = 1;
                if (ioctlsocket(endpoint, FIONBIO, &mode) != 0)
                {
                    closeSocket(endpoint);
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to get socket flags");
                }
#else
                const auto flags = fcntl(endpoint, F_GETFL, 0);
                if (flags == -1)
                {
                    closeSocket(endpoint);
                    throw std::system_error(errno, std::system_category(), "Failed to get socket flags");
                }

                if (fcntl(endpoint, F_SETFL, flags | O_NONBLOCK) == -1)
                {
                    closeSocket(endpoint);
                    throw std::system_error(errno, std::system_category(), "Failed to set socket flags");
                }
#endif

#if defined(__APPLE__)
                const int value = 1;
                if (setsockopt(endpoint, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value)) == -1)
                {
                    closeSocket(endpoint);
                    throw std::system_error(errno, std::system_category(), "Failed to set socket option");
                }
#endif
            }

            ~Socket()
            {
                if (endpoint != invalid) closeSocket(endpoint);
            }

            Socket(Socket&& other) noexcept:
                endpoint(other.endpoint)
            {
                other.endpoint = invalid;
            }

            Socket& operator=(Socket&& other) noexcept
            {
                if (&other == this) return *this;
                if (endpoint != invalid) closeSocket(endpoint);
                endpoint = other.endpoint;
                other.endpoint = invalid;
                return *this;
            }

            void connect(const struct sockaddr* address, socklen_t addressSize, const std::int64_t timeout)
            {
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(endpoint, &writeSet);

                timeval selectTimeout{
                    static_cast<decltype(timeval::tv_sec)>(timeout / 1000),
                    static_cast<decltype(timeval::tv_usec)>((timeout % 1000) * 1000)
                };

#ifdef _WIN32
                auto result = ::connect(endpoint, address, addressSize);
                while (result == -1 && WSAGetLastError() == WSAEINTR)
                    result = ::connect(endpoint, address, addressSize);

                if (result == -1)
                {
                    if (WSAGetLastError() != WSAEWOULDBLOCK)
                    {
                        auto count = select(0, nullptr, &writeSet, nullptr,
                                            (timeout >= 0) ? &selectTimeout : nullptr);

                        while (count == -1 && WSAGetLastError() == WSAEINTR)
                            count = select(0, nullptr, &writeSet, nullptr,
                                           (timeout >= 0) ? &selectTimeout : nullptr);

                        if (count == -1)
                            throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to select socket");
                        else if (count == 0)
                            throw ResponseError("Request timed out");

                        int socketError;
                        socklen_t optionLength = sizeof(socketError);
                        if (getsockopt(endpoint, SOL_SOCKET, SO_ERROR, &socketError, &optionLength) == -1)
                            throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to get socket option");

                        if (socketError != 0)
                            throw std::system_error(socketError, std::system_category(), "Failed to connect");
                    }
                    else
                        throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to connect");
                }
#else
                auto result = ::connect(endpoint, address, addressSize);
                while (result == -1 && errno == EINTR)
                    result = ::connect(endpoint, address, addressSize);

                if (result == -1)
                {
                    if (errno == EINPROGRESS)
                    {
                        auto count = select(endpoint + 1, nullptr, &writeSet, nullptr,
                                            (timeout >= 0) ? &selectTimeout : nullptr);

                        while (count == -1 && errno == EINTR)
                            count = select(endpoint + 1, nullptr, &writeSet, nullptr,
                                           (timeout >= 0) ? &selectTimeout : nullptr);

                        if (count == -1)
                            throw std::system_error(errno, std::system_category(), "Failed to select socket");
                        else if (count == 0)
                            throw ResponseError("Request timed out");

                        int socketError;
                        socklen_t optionLength = sizeof(socketError);
                        if (getsockopt(endpoint, SOL_SOCKET, SO_ERROR, &socketError, &optionLength) == -1)
                            throw std::system_error(errno, std::system_category(), "Failed to get socket option");

                        if (socketError != 0)
                            throw std::system_error(socketError, std::system_category(), "Failed to connect");
                    }
                    else
                        throw std::system_error(errno, std::system_category(), "Failed to connect");
                }
#endif
            }

            std::size_t send(const void* buffer, std::size_t length, const std::int64_t timeout)
            {
                fd_set writeSet;
                FD_ZERO(&writeSet);
                FD_SET(endpoint, &writeSet);

                timeval selectTimeout{
                    static_cast<decltype(timeval::tv_sec)>(timeout / 1000),
                    static_cast<decltype(timeval::tv_usec)>((timeout % 1000) * 1000)
                };

#ifdef _WIN32
                auto count = select(0, nullptr, &writeSet, nullptr,
                                    (timeout >= 0) ? &selectTimeout : nullptr);

                while (count == -1 && WSAGetLastError() == WSAEINTR)
                    count = select(0, nullptr, &writeSet, nullptr,
                                   (timeout >= 0) ? &selectTimeout : nullptr);

                if (count == -1)
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to select socket");
                else if (count == 0)
                    throw ResponseError("Request timed out");

                auto result = ::send(endpoint, reinterpret_cast<const char*>(buffer),
                                     static_cast<int>(length), 0);

                while (result == -1 && WSAGetLastError() == WSAEINTR)
                    result = ::send(endpoint, reinterpret_cast<const char*>(buffer),
                                    static_cast<int>(length), 0);

                if (result == -1)
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to send data");
#else
                auto count = select(endpoint + 1, nullptr, &writeSet, nullptr,
                                    (timeout >= 0) ? &selectTimeout : nullptr);

                while (count == -1 && errno == EINTR)
                    count = select(endpoint + 1, nullptr, &writeSet, nullptr,
                                   (timeout >= 0) ? &selectTimeout : nullptr);

                if (count == -1)
                    throw std::system_error(errno, std::system_category(), "Failed to select socket");
                else if (count == 0)
                    throw ResponseError("Request timed out");

                auto result = ::send(endpoint, reinterpret_cast<const char*>(buffer),
                                     length, noSignal);

                while (result == -1 && errno == EINTR)
                    result = ::send(endpoint, reinterpret_cast<const char*>(buffer),
                                    length, noSignal);

                if (result == -1)
                    throw std::system_error(errno, std::system_category(), "Failed to send data");
#endif
                return static_cast<std::size_t>(result);
            }

            std::size_t recv(void* buffer, std::size_t length, const std::int64_t timeout)
            {
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(endpoint, &readSet);

                timeval selectTimeout{
                    static_cast<decltype(timeval::tv_sec)>(timeout / 1000),
                    static_cast<decltype(timeval::tv_usec)>((timeout % 1000) * 1000)
                };
#ifdef _WIN32
                auto count = select(0, &readSet, nullptr, nullptr,
                                    (timeout >= 0) ? &selectTimeout : nullptr);

                while (count == -1 && WSAGetLastError() == WSAEINTR)
                    count = select(0, &readSet, nullptr, nullptr,
                                   (timeout >= 0) ? &selectTimeout : nullptr);

                if (count == -1)
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to select socket");
                else if (count == 0)
                    throw ResponseError("Request timed out");

                auto result = ::recv(endpoint, reinterpret_cast<char*>(buffer),
                                     static_cast<int>(length), 0);

                while (result == -1 && WSAGetLastError() == WSAEINTR)
                    result = ::recv(endpoint, reinterpret_cast<char*>(buffer),
                                    static_cast<int>(length), 0);

                if (result == -1)
                    throw std::system_error(WSAGetLastError(), std::system_category(), "Failed to read data");
#else
                auto count = select(endpoint + 1, &readSet, nullptr, nullptr,
                                    (timeout >= 0) ? &selectTimeout : nullptr);

                while (count == -1 && errno == EINTR)
                    count = select(endpoint + 1, &readSet, nullptr, nullptr,
                                   (timeout >= 0) ? &selectTimeout : nullptr);

                if (count == -1)
                    throw std::system_error(errno, std::system_category(), "Failed to select socket");
                else if (count == 0)
                    throw ResponseError("Request timed out");

                auto result = ::recv(endpoint, reinterpret_cast<char*>(buffer),
                                     length, noSignal);

                while (result == -1 && errno == EINTR)
                    result = ::recv(endpoint, reinterpret_cast<char*>(buffer),
                                    length, noSignal);

                if (result == -1)
                    throw std::system_error(errno, std::system_category(), "Failed to read data");
#endif
                return static_cast<std::size_t>(result);
            }

            operator Type() const noexcept { return endpoint; }

        private:
            Type endpoint = invalid;
        };
    }

    inline std::string urlEncode(const std::string& str)
    {
        constexpr std::array<char, 16> hexChars = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

        std::string result;

        for (auto i = str.begin(); i != str.end(); ++i)
        {
            const std::uint8_t cp = *i & 0xFF;

            if ((cp >= 0x30 && cp <= 0x39) || // 0-9
                (cp >= 0x41 && cp <= 0x5A) || // A-Z
                (cp >= 0x61 && cp <= 0x7A) || // a-z
                cp == 0x2D || cp == 0x2E || cp == 0x5F) // - . _
                result += static_cast<char>(cp);
            else if (cp <= 0x7F) // length = 1
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
            else if ((cp >> 5) == 0x06) // length = 2
            {
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
            }
            else if ((cp >> 4) == 0x0E) // length = 3
            {
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
            }
            else if ((cp >> 3) == 0x1E) // length = 4
            {
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
                if (++i == str.end()) break;
                result += std::string("%") + hexChars[(*i & 0xF0) >> 4] + hexChars[*i & 0x0F];
            }
        }

        return result;
    }

    struct Response final
    {
        enum Status
        {
            Continue = 100,
            SwitchingProtocol = 101,
            Processing = 102,
            EarlyHints = 103,

            Ok = 200,
            Created = 201,
            Accepted = 202,
            NonAuthoritativeInformation = 203,
            NoContent = 204,
            ResetContent = 205,
            PartialContent = 206,
            MultiStatus = 207,
            AlreadyReported = 208,
            ImUsed = 226,

            MultipleChoice = 300,
            MovedPermanently = 301,
            Found = 302,
            SeeOther = 303,
            NotModified = 304,
            UseProxy = 305,
            TemporaryRedirect = 307,
            PermanentRedirect = 308,

            BadRequest = 400,
            Unauthorized = 401,
            PaymentRequired = 402,
            Forbidden = 403,
            NotFound = 404,
            MethodNotAllowed = 405,
            NotAcceptable = 406,
            ProxyAuthenticationRequired = 407,
            RequestTimeout = 408,
            Conflict = 409,
            Gone = 410,
            LengthRequired = 411,
            PreconditionFailed = 412,
            PayloadTooLarge = 413,
            UriTooLong = 414,
            UnsupportedMediaType = 415,
            RangeNotSatisfiable = 416,
            ExpectationFailed = 417,
            MisdirectedRequest = 421,
            UnprocessableEntity = 422,
            Locked = 423,
            FailedDependency = 424,
            TooEarly = 425,
            UpgradeRequired = 426,
            PreconditionRequired = 428,
            TooManyRequests = 429,
            RequestHeaderFieldsTooLarge = 431,
            UnavailableForLegalReasons = 451,

            InternalServerError = 500,
            NotImplemented = 501,
            BadGateway = 502,
            ServiceUnavailable = 503,
            GatewayTimeout = 504,
            HttpVersionNotSupported = 505,
            VariantAlsoNegotiates = 506,
            InsufficientStorage = 507,
            LoopDetected = 508,
            NotExtended = 510,
            NetworkAuthenticationRequired = 511
        };

        int status = 0;
        std::vector<std::string> headers;
        std::vector<std::uint8_t> body;
    };

    class Request final
    {
    public:
        explicit Request(const std::string& url,
                         InternetProtocol protocol = InternetProtocol::V4):
            internetProtocol(protocol)
        {
            const auto schemeEndPosition = url.find("://");

            if (schemeEndPosition != std::string::npos)
            {
                scheme = url.substr(0, schemeEndPosition);
                path = url.substr(schemeEndPosition + 3);
            }
            else
            {
                scheme = "http";
                path = url;
            }

            const auto fragmentPosition = path.find('#');

            // remove the fragment part
            if (fragmentPosition != std::string::npos)
                path.resize(fragmentPosition);

            const auto pathPosition = path.find('/');

            if (pathPosition == std::string::npos)
            {
                domain = path;
                path = "/";
            }
            else
            {
                domain = path.substr(0, pathPosition);
                path = path.substr(pathPosition);
            }

            const auto portPosition = domain.find(':');

            if (portPosition != std::string::npos)
            {
                port = domain.substr(portPosition + 1);
                domain.resize(portPosition);
            }
            else
                port = "80";
        }

        Response send(const std::string& method,
                      const std::map<std::string, std::string>& parameters,
                      const std::vector<std::string>& headers = {},
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1})
        {
            std::string body;
            bool first = true;

            for (const auto& parameter : parameters)
            {
                if (!first) body += "&";
                first = false;

                body += urlEncode(parameter.first) + "=" + urlEncode(parameter.second);
            }

            return send(method, body, headers, timeout);
        }

        Response send(const std::string& method = "GET",
                      const std::string& body = "",
                      const std::vector<std::string>& headers = {},
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1})
        {
            return send(method,
                        std::vector<uint8_t>(body.begin(), body.end()),
                        headers,
                        timeout);
        }

        Response send(const std::string& method,
                      const std::vector<uint8_t>& body,
                      const std::vector<std::string>& headers,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds{-1})
        {
            const auto stopTime = std::chrono::steady_clock::now() + timeout;

            if (scheme != "http")
                throw RequestError("Only HTTP scheme is supported");

            addrinfo hints = {};
            hints.ai_family = getAddressFamily(internetProtocol);
            hints.ai_socktype = SOCK_STREAM;

            addrinfo* info;
            if (getaddrinfo(domain.c_str(), port.c_str(), &hints, &info) != 0)
                throw std::system_error(getLastError(), std::system_category(), "Failed to get address info of " + domain);

            std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addressInfo(info, freeaddrinfo);

            // RFC 7230, 3.1.1. Request Line
            std::string headerData = method + " " + path + " HTTP/1.1\r\n";

            for (const auto& header : headers)
                headerData += header + "\r\n";

            // RFC 7230, 3.2.  Header Fields
            headerData += "Host: " + domain + "\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "\r\n";

            std::vector<uint8_t> requestData(headerData.begin(), headerData.end());
            requestData.insert(requestData.end(), body.begin(), body.end());

            Socket socket(internetProtocol);

            // take the first address from the list
            socket.connect(addressInfo->ai_addr, static_cast<socklen_t>(addressInfo->ai_addrlen),
                           (timeout.count() >= 0) ? getRemainingMilliseconds(stopTime) : -1);

            auto remaining = requestData.size();
            auto sendData = requestData.data();

            // send the request
            while (remaining > 0)
            {
                const auto size = socket.send(sendData, remaining,
                                              (timeout.count() >= 0) ? getRemainingMilliseconds(stopTime) : -1);
                remaining -= size;
                sendData += size;
            }

            std::array<std::uint8_t, 4096> tempBuffer;
            constexpr std::array<std::uint8_t, 2> crlf = {'\r', '\n'};
            Response response;
            std::vector<std::uint8_t> responseData;
            bool parsedStatusLine = false;
            bool parsedHeaders = false;
            bool contentLengthReceived = false;
            std::size_t contentLength = 0;
            bool chunkedResponse = false;
            std::size_t expectedChunkSize = 0;
            bool removeCrlfAfterChunk = false;

            // read the response
            for (;;)
            {
                const auto size = socket.recv(tempBuffer.data(), tempBuffer.size(),
                                              (timeout.count() >= 0) ? getRemainingMilliseconds(stopTime) : -1);
                if (size == 0) break; // disconnected

                responseData.insert(responseData.end(), tempBuffer.begin(), tempBuffer.begin() + size);

                if (!parsedHeaders)
                    for (;;)
                    {
                        // RFC 7230, 3. Message Format
                        const auto i = std::search(responseData.begin(), responseData.end(), std::begin(crlf), std::end(crlf));

                        // didn't find a newline
                        if (i == responseData.end()) break;

                        const std::string line(responseData.begin(), i);
                        responseData.erase(responseData.begin(), i + 2);

                        // empty line indicates the end of the header section
                        if (line.empty())
                        {
                            parsedHeaders = true;
                            break;
                        }
                        else if (!parsedStatusLine) // RFC 7230, 3.1.2. Status Line
                        {
                            parsedStatusLine = true;
                            std::size_t partNum = 0;

                            // tokenize the status line
                            for (auto beginIterator = line.begin(); beginIterator != line.end();)
                            {
                                const auto endIterator = std::find(beginIterator, line.end(), ' ');
                                const std::string part{beginIterator, endIterator};

                                if (++partNum == 2) response.status = std::stoi(part);

                                if (endIterator == line.end()) break;
                                beginIterator = endIterator + 1;
                            }
                        }
                        else // RFC 7230, 3.2.  Header Fields
                        {
                            response.headers.push_back(line);

                            const auto loumnPosition = line.find(':');

                            if (loumnPosition == std::string::npos)
                                throw ResponseError("Invalid header: " + line);

                            const auto headerName = line.substr(0, loumnPosition);
                            auto headerValue = line.substr(loumnPosition + 1);

                            // RFC 7230, Appendix B. Collected ABNF
                            auto isNotWhiteSpace = [](char c){
                                return c != ' ' && c != '\t';
                            };

                            // ltrim
                            headerValue.erase(headerValue.begin(), std::find_if(headerValue.begin(), headerValue.end(), isNotWhiteSpace));

                            // rtrim
                            headerValue.erase(std::find_if(headerValue.rbegin(), headerValue.rend(), isNotWhiteSpace).base(), headerValue.end());

                            if (headerName == "Content-Length")
                            {
                                contentLength = std::stoul(headerValue);
                                contentLengthReceived = true;
                                response.body.reserve(contentLength);
                            }
                            else if (headerName == "Transfer-Encoding")
                            {
                                if (headerValue == "chunked")
                                    chunkedResponse = true;
                                else
                                    throw ResponseError("Unsupported transfer encoding: " + headerValue);
                            }
                        }
                    }

                if (parsedHeaders)
                {
                    // Content-Length must be ignored if Transfer-Encoding is received
                    if (chunkedResponse)
                    {
                        bool dataReceived = false;
                        for (;;)
                        {
                            if (expectedChunkSize > 0)
                            {
                                const auto toWrite = (std::min)(expectedChunkSize, responseData.size());
                                response.body.insert(response.body.end(), responseData.begin(), responseData.begin() + static_cast<ptrdiff_t>(toWrite));
                                responseData.erase(responseData.begin(), responseData.begin() + static_cast<ptrdiff_t>(toWrite));
                                expectedChunkSize -= toWrite;

                                if (expectedChunkSize == 0) removeCrlfAfterChunk = true;
                                if (responseData.empty()) break;
                            }
                            else
                            {
                                if (removeCrlfAfterChunk)
                                {
                                    if (responseData.size() >= 2)
                                    {
                                        removeCrlfAfterChunk = false;
                                        responseData.erase(responseData.begin(), responseData.begin() + 2);
                                    }
                                    else break;
                                }

                                const auto i = std::search(responseData.begin(), responseData.end(), std::begin(crlf), std::end(crlf));

                                if (i == responseData.end()) break;

                                const std::string line(responseData.begin(), i);
                                responseData.erase(responseData.begin(), i + 2);

                                expectedChunkSize = std::stoul(line, nullptr, 16);

                                if (expectedChunkSize == 0)
                                {
                                    dataReceived = true;
                                    break;
                                }
                            }
                        }

                        if (dataReceived)
                            break;
                    }
                    else
                    {
                        response.body.insert(response.body.end(), responseData.begin(), responseData.end());
                        responseData.clear();

                        // got the whole content
                        if (contentLengthReceived && response.body.size() >= contentLength)
                            break;
                    }
                }
            }

            return response;
        }

    private:
        static std::int64_t getRemainingMilliseconds(std::chrono::steady_clock::time_point time)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto remainingTime = std::chrono::duration_cast<std::chrono::milliseconds>(time - now);
            const auto remainingMilliseconds = remainingTime.count() ? remainingTime.count() : 0;
            return remainingMilliseconds;
        }

#ifdef _WIN32
        WinSock winSock;
#endif
        InternetProtocol internetProtocol;
        std::string scheme;
        std::string domain;
        std::string port;
        std::string path;
    };
}

#endif
