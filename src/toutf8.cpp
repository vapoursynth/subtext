#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

#include <iconv.h>

#include "common.h"


namespace {

class IconvDescriptor {
    iconv_t descriptor;

public:
    IconvDescriptor(const char *to, const char *from) : descriptor(iconv_open(to, from)) { }

    ~IconvDescriptor() {
        if (*this)
            iconv_close(descriptor);
    }

    IconvDescriptor(const IconvDescriptor &) = delete;
    IconvDescriptor &operator=(const IconvDescriptor &) = delete;

    explicit operator bool() const { return descriptor != (iconv_t)-1; }
    operator iconv_t() const { return descriptor; }
};

} // namespace


// Function stolen from libass.
static std::optional<std::string> subRecode(const std::string &data, const std::string &codepage, std::string &error) {
    IconvDescriptor descriptor("UTF-8", codepage.c_str());
    if (!descriptor) {
        error = "failed to open iconv descriptor.";

        return {};
    }

    // Growing by a fixed amount when the input is empty guarantees progress.
    const size_t grow = data.size() ? data.size() : 64;

    std::string converted(data.size() + 1, '\0');

    // iconv wants a mutable pointer even though it only reads the input.
    char *ip = const_cast<char *>(data.data());
    char *op = converted.data();
    size_t ileft = data.size();
    size_t oleft = converted.size();
    bool clear = false;

    while (true) {
        size_t rc;

        if (ileft) {
            rc = iconv(descriptor, &ip, &ileft, &op, &oleft);
        } else {
            // Clear the conversion state and leave.
            clear = true;
            rc = iconv(descriptor, nullptr, nullptr, &op, &oleft);
        }

        if (rc != (size_t)-1) {
            if (clear)
                break;

            continue;
        }

        if (errno != E2BIG) {
            error = "failed to convert subtitle file to UTF8, at byte " + std::to_string(ip - data.data()) + ".";

            return {};
        }

        size_t used = converted.size() - oleft;
        converted.resize(converted.size() + grow);
        op = converted.data() + used;
        oleft = converted.size() - used;
    }

    converted.resize(converted.size() - oleft);

    return converted;
}


static bool isUtf8(const std::string &charset) {
    std::string upper;

    for (char c : charset)
        upper += (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;

    return upper == "UTF8" || upper == "UTF-8";
}


std::optional<std::string> convertToUtf8(const std::string &file_name, const std::string &charset, std::string &error) {
    /* The name is UTF-8, which is not what a path is built from by default. */
    std::filesystem::path path = std::filesystem::u8path(file_name);

    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "failed to obtain the size of the subtitle file: " + ec.message() + ".";

        return {};
    }

    /* The whole file has to fit in memory and in a single read. */
    if (size > static_cast<uintmax_t>(SIZE_MAX) ||
        size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        error = "the subtitle file is too large.";

        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "failed to open subtitle file.";

        return {};
    }

    std::string contents(static_cast<size_t>(size), '\0');

    /* Short reads leave the stream in a failed state, so the amount that was
       read doesn't have to be compared against the size again. */
    file.read(contents.data(), static_cast<std::streamsize>(size));
    if (!file) {
        error = "failed to read the contents of the subtitle file.";

        return {};
    }

    if (!isUtf8(charset))
        return subRecode(contents, charset, error);

    return contents;
}
