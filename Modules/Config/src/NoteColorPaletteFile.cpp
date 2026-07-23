#include "config/NoteColorPaletteFile.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <system_error>

namespace MMM::Config
{

bool exportNoteColorPaletteFile(const std::filesystem::path&  path,
                                const NoteColorPaletteScheme& scheme)
{
    if ( path.empty() ) {
        XERROR("Cannot export note color palette to an empty path");
        return false;
    }

    const std::filesystem::path parent = path.parent_path();
    if ( !parent.empty() ) {
        std::error_code createDirectoryError;
        std::filesystem::create_directories(parent, createDirectoryError);
        if ( createDirectoryError ) {
            XERROR(
                "Failed to create note color palette export directory: {}. "
                "Error: {}",
                pathToUtf8(parent),
                createDirectoryError.message());
            return false;
        }
    }

    NoteColorPaletteFile paletteFile;
    paletteFile.scheme            = scheme;
    const nlohmann::json document = paletteFile;

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    bool writeSucceeded = false;
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if ( !output.is_open() ) {
            XERROR("Failed to open note color palette export file: {}",
                   pathToUtf8(temporaryPath));
            return false;
        }

        output << std::setw(4) << document << '\n';
        writeSucceeded = output.good();
    }

    if ( !writeSucceeded ) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        XERROR("Failed to write note color palette export file: {}",
               pathToUtf8(temporaryPath));
        return false;
    }

    std::error_code replaceError;
    std::filesystem::rename(temporaryPath, path, replaceError);
    if ( replaceError ) {
        std::error_code copyError;
        std::filesystem::copy_file(
            temporaryPath,
            path,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        if ( copyError ) {
            XERROR(
                "Failed to replace note color palette export file: {}. "
                "Error: {}",
                pathToUtf8(path),
                copyError.message());
            return false;
        }
    }

    XINFO("Note color palette exported: {}", pathToUtf8(path));
    return true;
}

}  // namespace MMM::Config
