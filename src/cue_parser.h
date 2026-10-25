#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bin2iso
{

    // Track mode as declared in the CUE sheet.
    enum class TrackMode
    {
        Audio,      // AUDIO
        Mode1_2048, // MODE1/2048  (already cooked user data)
        Mode1_2352, // MODE1/2352  (raw sector, 16-byte header)
        Mode2_2048, // MODE2/2048
        Mode2_2324, // MODE2/2324  (Form 2, no sub-header stripping for ISO)
        Mode2_2336, // MODE2/2336  (sub-header + data, no sync/header)
        Mode2_2352, // MODE2/2352  (raw XA sector, 24-byte header for Form 1)
        Unknown
    };

    struct Index
    {
        int number = 0;     // INDEX number (0 or 1 typically)
        uint64_t frame = 0; // absolute position in frames (sectors) from start of file
    };

    struct Track
    {
        int number = 0;
        TrackMode mode = TrackMode::Unknown;
        std::string modeText; // original text, e.g. "MODE1/2352"
        std::string fileName; // the FILE this track belongs to (name as in cue)
        std::vector<Index> indices;

        // Bytes per raw sector in the BIN for this track.
        uint32_t rawSectorSize() const;
        bool isData() const;
        // Offset/length of the 2048-byte user payload within a raw sector.
        // Returns false if this mode cannot be reduced to a 2048-byte ISO sector.
        bool userDataLayout(uint32_t& offset, uint32_t& length) const;
    };

    struct CueFile
    {
        std::string binFileName;            // first FILE "...." BINARY (name only, as in cue)
        std::vector<std::string> fileNames; // all FILE entries, in order
        std::vector<Track> tracks;

        // True when the cue references more than one FILE (multi-bin layout).
        bool isMultiFile() const
        {
            return fileNames.size() > 1;
        }
    };

    struct CueParseResult
    {
        bool ok = false;
        std::string error;
        CueFile cue;
    };

    // Parse the text of a .cue sheet.
    CueParseResult parseCue(const std::string& cueText);

    // Convert "MM:SS:FF" (minutes:seconds:frames, 75 frames/sec) to absolute frames.
    uint64_t msfToFrames(const std::string& msf, bool& ok);

    const char* trackModeName(TrackMode m);

} // namespace bin2iso
