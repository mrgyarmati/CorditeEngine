#pragma once

/// @file
/// Noticing that the files under a directory change.
///
/// A DirectoryWatcher looks at a directory on a thread of its own every so often - which
/// files there are, how large each is and when it was last written - and gathers what was
/// added, removed or written until the program asks (takeChanges()). It polls. The
/// notifications of the systems differ from one another, miss what happens on some file
/// systems (network drives) and cannot be tested the same way everywhere; a look at a few
/// thousand files costs milliseconds, and is not taken on the thread that asks.
///
/// @code
/// DirectoryWatcher watcher({.directory = project.assetDirectory()});
/// // Every frame:
/// if (!watcher.takeChanges().empty()) {
///     CORDITE_TRY(assets.refresh());
///     watcher.rebaseline();   // what the refresh wrote is no change
/// }
/// @endcode

#include "cordite/core/Base.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cordite {

/// What a file is, as far as noticing that it changed goes.
struct FileStamp {
    uintmax_t size = 0;
    std::filesystem::file_time_type written;

    bool operator==(const FileStamp&) const = default;
};

/// The files under `directory` - not those in directories whose names begin with a dot, as
/// the assets do not count them either - by their paths relative to it, with forward
/// slashes. What cannot be read is left out; a directory that is not there has no files.
[[nodiscard]] std::map<std::string, FileStamp> stampFiles(const std::filesystem::path& directory);

/// The paths that one of two lists of files has and the other has not, or both have with
/// other stamps: what was added, removed or written between them. Sorted.
[[nodiscard]] std::vector<std::string> changedFiles(const std::map<std::string, FileStamp>& before,
                                                    const std::map<std::string, FileStamp>& after);

struct DirectoryWatcherDesc {
    std::filesystem::path directory;
    /// How long the watcher waits between two looks.
    std::chrono::milliseconds interval{500};
};

class DirectoryWatcher {
public:
    /// Takes the files as they are now for what they were, and starts looking.
    explicit DirectoryWatcher(DirectoryWatcherDesc desc);
    /// Stops looking, after a look that is under way.
    ~DirectoryWatcher();
    CORDITE_NON_MOVABLE(DirectoryWatcher);

    /// What was added, removed or written since the last call or rebaseline(): paths
    /// relative to the directory, with forward slashes, sorted, each once.
    [[nodiscard]] std::vector<std::string> takeChanges();

    /// Takes the files as they are now for what they were - on the calling thread - and
    /// forgets what changed before: for after the program wrote into the directory itself.
    /// A look that was under way meanwhile counts for nothing.
    void rebaseline();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
