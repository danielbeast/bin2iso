#include <cstdio>
#include <iostream>
#include <string>

#include "converter.h"

using namespace bin2iso;

namespace
{

    void printUsage(const char* exe)
    {
        std::cout << "bin2iso - convert BIN/CUE CD images to ISO\n\n"
                     "Usage:\n"
                     "  "
                  << exe
                  << " <input.cue> [output.iso] [options]\n\n"
                     "Arguments:\n"
                     "  input.cue     Path to the .cue sheet describing the image.\n"
                     "  output.iso    Output ISO path. Defaults to the cue name with .iso.\n\n"
                     "Options:\n"
                     "  --bin <path>  Override the BIN path from the cue sheet.\n"
                     "  -q, --quiet   Suppress the progress bar.\n"
                     "  -h, --help    Show this help.\n\n"
                     "Notes:\n"
                     "  Only data tracks (MODE1/MODE2) are written. Audio tracks cannot be\n"
                     "  represented inside an ISO and are reported as warnings.\n";
    }

    std::string defaultIsoFromCue(const std::string& cuePath)
    {
        size_t dot = cuePath.find_last_of('.');
        size_t slash = cuePath.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        {
            return cuePath.substr(0, dot) + ".iso";
        }
        return cuePath + ".iso";
    }

    void drawProgress(uint64_t done, uint64_t total)
    {
        if (total == 0)
        {
            return;
        }
        const int width = 40;
        double frac = static_cast<double>(done) / static_cast<double>(total);
        if (frac > 1.0)
        {
            frac = 1.0;
        }
        int filled = static_cast<int>(frac * width);
        std::fputc('\r', stdout);
        std::fputc('[', stdout);
        for (int i = 0; i < width; ++i)
        {
            std::fputc(i < filled ? '#' : '-', stdout);
        }
        std::printf("] %3d%%", static_cast<int>(frac * 100));
        std::fflush(stdout);
    }

} // namespace

int main(int argc, char** argv)
{
    std::string cuePath, isoPath, binOverride;
    bool quiet = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            printUsage(argv[0]);
            return 0;
        }
        else if (a == "-q" || a == "--quiet")
        {
            quiet = true;
        }
        else if (a == "--bin")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Error: --bin requires a path.\n";
                return 2;
            }
            binOverride = argv[++i];
        }
        else if (!a.empty() && a[0] == '-')
        {
            std::cerr << "Unknown option: " << a << "\n";
            return 2;
        }
        else if (cuePath.empty())
        {
            cuePath = a;
        }
        else if (isoPath.empty())
        {
            isoPath = a;
        }
        else
        {
            std::cerr << "Unexpected argument: " << a << "\n";
            return 2;
        }
    }

    if (cuePath.empty())
    {
        printUsage(argv[0]);
        return 2;
    }
    if (isoPath.empty())
    {
        isoPath = defaultIsoFromCue(cuePath);
    }

    ConvertOptions opts;
    opts.cuePath = cuePath;
    opts.binPath = binOverride;
    opts.isoPath = isoPath;

    std::cout << "Converting: " << cuePath << "\n"
              << "Output:     " << isoPath << "\n";

    ProgressFn progress;
    if (!quiet)
    {
        progress = [](uint64_t done, uint64_t total)
        {
            drawProgress(done, total);
            return true;
        };
    }

    ConvertResult r = convert(opts, progress);
    if (!quiet)
    {
        std::cout << "\n";
    }

    if (!r.warnings.empty())
    {
        std::cout << r.warnings;
    }

    if (!r.ok)
    {
        std::cerr << "FAILED: " << r.error << "\n";
        return 1;
    }

    std::cout << "Done. Wrote " << r.sectorsWritten << " sectors (" << r.bytesWritten << " bytes) to " << isoPath
              << "\n";
    return 0;
}
