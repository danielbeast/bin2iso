#include "converter.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace bin2iso
{

    bool readTextFile(const std::string& path, std::string& out, std::string& error)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            error = "Cannot open file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    std::string resolveBinPath(const std::string& cuePath, const std::string& binName)
    {
        // If the cue references an absolute-looking path, honour it.
        bool absolute = false;
        if (!binName.empty())
        {
            if (binName[0] == '/' || binName[0] == '\\')
            {
                absolute = true;
            }
            if (binName.size() > 1 && binName[1] == ':')
            {
                absolute = true; // Windows drive
            }
        }
        if (absolute)
        {
            return binName;
        }

        // Otherwise resolve relative to the cue file's directory.
        size_t slash = cuePath.find_last_of("/\\");
        if (slash == std::string::npos)
        {
            return binName;
        }
        return cuePath.substr(0, slash + 1) + binName;
    }

    namespace
    {

        uint64_t fileSize(std::ifstream& f)
        {
            const std::streampos cur = f.tellg();
            f.seekg(0, std::ios::end);
            const std::streampos end = f.tellg();
            f.seekg(cur, std::ios::beg);
            return static_cast<uint64_t>(end);
        }

    } // namespace

    ConvertResult convert(const ConvertOptions& opts, const ProgressFn& progress)
    {
        ConvertResult result;

        // --- Load and parse the cue sheet ---
        std::string cueText, err;
        if (!readTextFile(opts.cuePath, cueText, err))
        {
            result.error = err;
            return result;
        }
        CueParseResult parsed = parseCue(cueText);
        if (!parsed.ok)
        {
            result.error = "CUE parse error: " + parsed.error;
            return result;
        }
        const CueFile& cue = parsed.cue;

        // --- Find the first data track and warn about the rest ---
        std::ostringstream warn;
        int dataTrackCount = 0;
        const Track* dataTrack = nullptr;
        size_t dataTrackIdx = 0;
        for (size_t i = 0; i < cue.tracks.size(); ++i)
        {
            const Track& t = cue.tracks[i];
            if (t.isData())
            {
                ++dataTrackCount;
                if (!dataTrack)
                {
                    dataTrack = &t;
                    dataTrackIdx = i;
                }
            }
            else if (opts.keepAudioWarning)
            {
                warn << "Track " << t.number << " (" << trackModeName(t.mode)
                     << ") is audio and cannot be placed in an ISO; skipped.\n";
            }
        }

        if (!dataTrack)
        {
            result.error = "No data track found; nothing to convert to ISO "
                           "(the image appears to be audio-only).";
            return result;
        }
        if (dataTrackCount > 1)
        {
            warn << "Multiple data tracks present; only the first (Track " << dataTrack->number
                 << ") is written to the ISO.\n";
        }

        // In a multi-file cue each FILE holds one track's data starting at frame 0,
        // so the data track lives in its own bin. In a single-file cue every track
        // shares one bin and frame offsets are absolute within it.
        const bool multiFile = cue.isMultiFile();

        // --- Resolve the bin path ---
        std::string binPath = opts.binPath;
        if (binPath.empty())
        {
            const std::string& wantFile =
                (multiFile && !dataTrack->fileName.empty()) ? dataTrack->fileName : cue.binFileName;
            binPath = resolveBinPath(opts.cuePath, wantFile);
        }

        std::ifstream in(binPath, std::ios::binary);
        if (!in)
        {
            result.error = "Cannot open BIN file: " + binPath;
            return result;
        }
        const uint64_t binBytes = fileSize(in);

        uint32_t userOffset = 0, userLen = 0;
        if (!dataTrack->userDataLayout(userOffset, userLen))
        {
            result.error = std::string("Track mode ") + trackModeName(dataTrack->mode) +
                           " cannot be converted to a 2048-byte ISO sector.";
            return result;
        }
        const uint32_t rawSize = dataTrack->rawSectorSize();

        // Start frame of a track = its INDEX 01 if present, else its first index.
        auto trackStartFrame = [](const Track& t, bool& found) -> uint64_t
        {
            found = false;
            uint64_t frame = 0;
            for (const Index& idx : t.indices)
            {
                if (!found)
                {
                    frame = idx.frame;
                    found = true;
                }
                if (idx.number == 1)
                {
                    return idx.frame;
                }
            }
            return frame;
        };

        // Earliest index of a track (INDEX 00 pregap if present, else INDEX 01).
        // Used to bound the preceding data track so its pregap is not emitted.
        auto trackLowestFrame = [](const Track& t, bool& found) -> uint64_t
        {
            found = false;
            uint64_t frame = 0;
            for (const Index& idx : t.indices)
            {
                if (!found || idx.frame < frame)
                {
                    frame = idx.frame;
                    found = true;
                }
            }
            return frame;
        };

        // --- Determine where this track starts in the bin (frame of INDEX 01) ---
        bool haveStart = false;
        const uint64_t startFrame = trackStartFrame(*dataTrack, haveStart);
        if (!haveStart)
        {
            result.error = "Data track has no INDEX; cannot locate its start.";
            return result;
        }
        const uint64_t startByte = startFrame * rawSize;
        if (startByte >= binBytes)
        {
            result.error = "Track start offset is beyond the end of the BIN file.";
            return result;
        }

        // --- Determine where this track ends ---
        // The data track ends where the next track begins. On a mixed-mode disc the
        // following (audio) sectors must NOT be dumped into the ISO. If no track
        // follows, the track runs to the end of the BIN.
        uint64_t endFrame = 0;
        bool haveEnd = false;
        // Only bound against the next track when it shares the same bin file. In a
        // multi-file cue the following track is a separate file, so the data bin
        // ends at EOF on its own.
        if (!multiFile && dataTrackIdx + 1 < cue.tracks.size())
        {
            bool nextOk = false;
            // Bound at the next track's EARLIEST index so its pregap (INDEX 00)
            // belongs to the audio track, not the data ISO.
            const uint64_t nextStart = trackLowestFrame(cue.tracks[dataTrackIdx + 1], nextOk);
            if (nextOk && nextStart > startFrame)
            {
                endFrame = nextStart;
                haveEnd = true;
            }
        }

        const uint64_t availableBytes = binBytes - startByte;
        uint64_t sectorCount = availableBytes / rawSize;
        if (haveEnd)
        {
            const uint64_t trackFrames = endFrame - startFrame;
            if (trackFrames < sectorCount)
            {
                sectorCount = trackFrames;
            }
        }
        if (sectorCount == 0)
        {
            result.error = "BIN file has no complete sectors for this track.";
            return result;
        }

        // --- Open output ---
        std::ofstream out(opts.isoPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            result.error = "Cannot create output ISO: " + opts.isoPath;
            return result;
        }

        in.seekg(static_cast<std::streamoff>(startByte), std::ios::beg);

        std::vector<char> sector(rawSize);
        const uint64_t totalBytes = sectorCount * rawSize;
        uint64_t processed = 0;

        for (uint64_t i = 0; i < sectorCount; ++i)
        {
            in.read(sector.data(), rawSize);
            if (in.gcount() != static_cast<std::streamsize>(rawSize))
            {
                break;
            }

            out.write(sector.data() + userOffset, userLen);
            if (!out)
            {
                result.error = "Write error while producing ISO (disk full?).";
                return result;
            }

            result.bytesWritten += userLen;
            ++result.sectorsWritten;
            processed += rawSize;

            if (progress && (i % 256 == 0 || i + 1 == sectorCount))
            {
                if (!progress(processed, totalBytes))
                {
                    result.error = "Conversion cancelled by user.";
                    out.close();
                    std::remove(opts.isoPath.c_str());
                    return result;
                }
            }
        }

        out.flush();
        if (!out)
        {
            result.error = "Failed to finalize ISO output.";
            return result;
        }

        result.warnings = warn.str();
        result.ok = true;
        return result;
    }

} // namespace bin2iso
