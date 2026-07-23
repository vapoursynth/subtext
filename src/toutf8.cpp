#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#include <iconv.h>



// Function stolen from libass.
static char *sub_recode(char *data, size_t *size, const char *codepage, char *error, size_t error_size) {
    iconv_t icdsc;
    const char *tocp = "UTF-8";
    char *outbuf;

    if ((icdsc = iconv_open(tocp, codepage)) == (iconv_t) (-1)) {
        snprintf(error, error_size, "failed to open iconv descriptor.");

        return NULL;
    }

    {
        // The extra byte is for the terminating null byte, which iconv doesn't
        // write itself. Growing by a fixed amount when the input is empty
        // guarantees progress.
        size_t grow = *size ? *size : 64;
        size_t osize = *size + 1;
        size_t ileft = *size;
        size_t oleft = osize - 1;
        char *ip;
        char *op;
        size_t rc;
        int clear = 0;

        outbuf = (char *)malloc(osize);
        if (!outbuf) {
            snprintf(error, error_size, "failed to allocate %zu bytes for iconv.", osize);

            goto out;
        }

        ip = data;
        op = outbuf;

        while (1) {
            if (ileft)
                rc = iconv(icdsc, &ip, &ileft, &op, &oleft);
            else {              // clear the conversion state and leave
                clear = 1;
                rc = iconv(icdsc, NULL, NULL, &op, &oleft);
            }
            if (rc == (size_t) (-1)) {
                if (errno == E2BIG) {
                    size_t offset = op - outbuf;
                    char *nbuf = (char *)realloc(outbuf, osize + grow);
                    if (!nbuf) {
                        free(outbuf);
                        outbuf = NULL;
                        snprintf(error, error_size, "failed to reallocate %zu bytes for iconv.", osize + grow);
                        goto out;
                    }
                    outbuf = nbuf;
                    op = outbuf + offset;
                    osize += grow;
                    oleft += grow;
                } else {
                    snprintf(error, error_size, "failed to convert subtitle file to UTF8, at byte %zu.", (size_t)(ip - data));
                    free(outbuf);
                    outbuf = NULL;
                    goto out;
                }
            } else if (clear)
                break;
        }
        outbuf[osize - oleft - 1] = 0;
        *size = osize - oleft - 1;
    }

out:
    if (icdsc != (iconv_t) (-1)) {
        (void) iconv_close(icdsc);
    }

    return outbuf;
}


static int isUtf8(const char *charset) {
    size_t length = strlen(charset);
    if (length != 4 && length != 5)
        return 0;

    char charset_upper[10] = { 0 };

    int diff = 'a' - 'A';
    for (size_t i = 0; i < length; i++) {
        charset_upper[i] = charset[i];
        if (charset[i] >= 'a' && charset[i] <= 'z')
            charset_upper[i] -= diff;
    }

    return strcmp(charset_upper, "UTF8") == 0 ||
           strcmp(charset_upper, "UTF-8") == 0;
}


char *convertToUtf8(const char *file_name, const char *charset, int64_t *file_size, char *error, size_t error_size) {
    /* The name is UTF-8, which is not what a path is built from by default. */
    std::filesystem::path path = std::filesystem::u8path(file_name);

    std::error_code ec;
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        snprintf(error, error_size, "failed to obtain the size of the subtitle file: %s.", ec.message().c_str());

        return nullptr;
    }

    /* The whole file has to fit in memory and in a single read. */
    if (size > static_cast<uintmax_t>(SIZE_MAX) ||
        size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        snprintf(error, error_size, "the subtitle file is too large.");

        return nullptr;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        snprintf(error, error_size, "failed to open subtitle file.");

        return nullptr;
    }

    *file_size = static_cast<int64_t>(size);

    /* Zero sized allocations are allowed to return nothing at all. */
    char *contents = (char *)malloc(size ? static_cast<size_t>(size) : 1);
    if (!contents) {
        snprintf(error, error_size, "failed to allocate %" PRIuMAX " bytes for the contents of the subtitle file.", size);

        return nullptr;
    }

    /* Short reads leave the stream in a failed state, so the amount that was
       read doesn't have to be compared against the size again. */
    file.read(contents, static_cast<std::streamsize>(size));
    if (!file) {
        snprintf(error, error_size, "failed to read the contents of the subtitle file.");

        free(contents);

        return nullptr;
    }


    if (charset && !isUtf8(charset)) {
        size_t recoded_size = static_cast<size_t>(size);
        char *utf8_contents = sub_recode(contents, &recoded_size, charset, error, error_size);
        free(contents);
        contents = utf8_contents;

        if (contents)
            *file_size = static_cast<int64_t>(recoded_size);
    }

    return contents;
}
