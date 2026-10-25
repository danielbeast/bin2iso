#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cue_parser.h"

namespace bin2iso
{

    struct ConvertOptions
    {
        std::string cuePath;          // path to the .cue file
        std::string binPath;          // path to the .bin file (resolved from cue dir if empty)
        std::string isoPath;          // output .iso path
        bool keepAudioWarning = true; // report audio tracks that cannot be converted
    };

    struct ConvertResult
    {
        bool ok = false;
        std::string error;
        std::string warnings; // non-fatal notes (e.g. skipped audio tracks)
        uint64_t sectorsWritten = 0;
        uint64_t bytesWritten = 0;
    };

    // Progress callback: (bytesDone, bytesTotal). Return false to cancel.
    using ProgressFn = std::function<bool(uint64_t, uint64_t)>;

    // Read a whole text file (used for the cue sheet). Returns false on failure.
    bool readTextFile(const std::string& path, std::string& out, std::string& error);

    // Given a cue path, derive the directory and join with the bin file name.
    std::string resolveBinPath(const std::string& cuePath, const std::string& binName);

    // Perform the conversion. The first data track is extracted to the ISO.
    ConvertResult convert(const ConvertOptions& opts, const ProgressFn& progress = {});

} // namespace bin2iso
