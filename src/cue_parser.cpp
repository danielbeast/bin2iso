#include "cue_parser.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace bin2iso
{

    namespace
    {

        std::string trim(const std::string& s)
        {
            size_t start = 0, end = s.size();
            while (start < end && std::isspace(static_cast<unsigned char>(s[start])))
            {
                ++start;
            }
            while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
            {
                --end;
            }
            return s.substr(start, end - start);
        }

        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c)
                           {
                               return static_cast<char>(std::toupper(c));
                           });
            return s;
        }

        // Split respecting double-quoted substrings so FILE "a b.bin" BINARY works.
        std::vector<std::string> tokenize(const std::string& line)
        {
            std::vector<std::string> out;
            std::string cur;
            bool inQuotes = false;
            for (char c : line)
            {
                if (c == '"')
                {
                    inQuotes = !inQuotes;
                    continue;
                }
                if (!inQuotes && std::isspace(static_cast<unsigned char>(c)))
                {
                    if (!cur.empty())
                    {
                        out.push_back(cur);
                        cur.clear();
                    }
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
            {
                out.push_back(cur);
            }
            return out;
        }

        TrackMode parseMode(const std::string& text)
        {
            const std::string m = upper(text);
            if (m == "AUDIO")
            {
                return TrackMode::Audio;
            }
            if (m == "MODE1/2048")
            {
                return TrackMode::Mode1_2048;
            }
            if (m == "MODE1/2352")
            {
                return TrackMode::Mode1_2352;
            }
            if (m == "MODE2/2048")
            {
                return TrackMode::Mode2_2048;
            }
            if (m == "MODE2/2324")
            {
                return TrackMode::Mode2_2324;
            }
            if (m == "MODE2/2336")
            {
                return TrackMode::Mode2_2336;
            }
            if (m == "MODE2/2352")
            {
                return TrackMode::Mode2_2352;
            }
            return TrackMode::Unknown;
        }

    } // namespace

    uint32_t Track::rawSectorSize() const
    {
        switch (mode)
        {
        case TrackMode::Mode1_2048:
            return 2048;
        case TrackMode::Mode2_2048:
            return 2048;
        case TrackMode::Mode2_2324:
            return 2324;
        case TrackMode::Mode2_2336:
            return 2336;
        case TrackMode::Mode1_2352:
        case TrackMode::Mode2_2352:
        case TrackMode::Audio:
            return 2352;
        default:
            return 2352;
        }
    }

    bool Track::isData() const
    {
        return mode != TrackMode::Audio && mode != TrackMode::Unknown;
    }

    bool Track::userDataLayout(uint32_t& offset, uint32_t& length) const
    {
        switch (mode)
        {
        case TrackMode::Mode1_2048:
        case TrackMode::Mode2_2048:
            offset = 0;
            length = 2048;
            return true;
        case TrackMode::Mode1_2352:
            // 12 sync + 4 header, then 2048 user data.
            offset = 16;
            length = 2048;
            return true;
        case TrackMode::Mode2_2352:
            // 12 sync + 4 header + 8 sub-header (Form 1), then 2048 user data.
            offset = 24;
            length = 2048;
            return true;
        case TrackMode::Mode2_2336:
            // 8 sub-header, then 2048 user data (Form 1).
            offset = 8;
            length = 2048;
            return true;
        default:
            return false; // Audio / Form 2 / unknown cannot map to a 2048 ISO sector.
        }
    }

    uint64_t msfToFrames(const std::string& msf, bool& ok)
    {
        ok = false;
        int mm = 0, ss = 0, ff = 0;
        char c1 = 0, c2 = 0;
        std::istringstream is(msf);
        is >> mm >> c1 >> ss >> c2 >> ff;
        if (is.fail() || c1 != ':' || c2 != ':')
        {
            return 0;
        }
        if (ss < 0 || ss >= 60 || ff < 0 || ff >= 75 || mm < 0)
        {
            return 0;
        }
        ok = true;
        return (static_cast<uint64_t>(mm) * 60 + ss) * 75 + ff;
    }

    const char* trackModeName(TrackMode m)
    {
        switch (m)
        {
        case TrackMode::Audio:
            return "AUDIO";
        case TrackMode::Mode1_2048:
            return "MODE1/2048";
        case TrackMode::Mode1_2352:
            return "MODE1/2352";
        case TrackMode::Mode2_2048:
            return "MODE2/2048";
        case TrackMode::Mode2_2324:
            return "MODE2/2324";
        case TrackMode::Mode2_2336:
            return "MODE2/2336";
        case TrackMode::Mode2_2352:
            return "MODE2/2352";
        default:
            return "UNKNOWN";
        }
    }

    CueParseResult parseCue(const std::string& cueText)
    {
        CueParseResult result;
        CueFile& cue = result.cue;

        std::istringstream stream(cueText);
        std::string line;
        Track* current = nullptr;
        std::string currentFile;
        int lineNo = 0;

        while (std::getline(stream, line))
        {
            ++lineNo;
            // Strip a trailing CR for files with CRLF line endings.
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            const std::string trimmed = trim(line);
            if (trimmed.empty())
            {
                continue;
            }

            std::vector<std::string> tokens = tokenize(trimmed);
            if (tokens.empty())
            {
                continue;
            }

            const std::string command = upper(tokens[0]);

            if (command == "FILE")
            {
                if (tokens.size() < 2)
                {
                    result.error = "FILE line missing filename (line " + std::to_string(lineNo) + ")";
                    return result;
                }
                currentFile = tokens[1];
                if (cue.binFileName.empty())
                {
                    cue.binFileName = currentFile;
                }
                cue.fileNames.push_back(currentFile);
            }
            else if (command == "TRACK")
            {
                if (tokens.size() < 3)
                {
                    result.error = "TRACK line malformed (line " + std::to_string(lineNo) + ")";
                    return result;
                }
                // TRACK <number> <mode>, e.g. "TRACK 01 MODE1/2352".
                Track track;
                track.number = std::atoi(tokens[1].c_str());
                track.modeText = tokens[2];
                track.mode = parseMode(tokens[2]);
                track.fileName = currentFile;
                cue.tracks.push_back(track);
                current = &cue.tracks.back();
            }
            else if (command == "INDEX")
            {
                if (!current)
                {
                    result.error = "INDEX without a TRACK (line " + std::to_string(lineNo) + ")";
                    return result;
                }
                if (tokens.size() < 3)
                {
                    result.error = "INDEX line malformed (line " + std::to_string(lineNo) + ")";
                    return result;
                }
                // INDEX <number> <mm:ss:ff>, e.g. "INDEX 01 00:02:00".
                Index index;
                index.number = std::atoi(tokens[1].c_str());
                bool ok = false;
                index.frame = msfToFrames(tokens[2], ok);
                if (!ok)
                {
                    result.error = "Bad INDEX timestamp '" + tokens[2] + "' (line " + std::to_string(lineNo) + ")";
                    return result;
                }
                current->indices.push_back(index);
            }
            // Other commands (REM, PERFORMER, TITLE, FLAGS, PREGAP, POSTGAP...) are ignored.
        }

        if (cue.binFileName.empty())
        {
            result.error = "No FILE entry found in cue sheet.";
            return result;
        }
        if (cue.tracks.empty())
        {
            result.error = "No TRACK entries found in cue sheet.";
            return result;
        }

        result.ok = true;
        return result;
    }

} // namespace bin2iso
